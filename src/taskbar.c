/*
 * taskbar.c - タスクバー (SPEC §4.8)
 *
 * 画面下端に置く 28px のパネル。左からスタートボタン、タスクボタン領域、
 * トレイ、時計。自分自身に _NET_WM_STRUT_PARTIAL を設定して、他ウィンドウの
 * 最大化領域を正しく確保する (§3.5.2)。
 *
 * 実装方針:
 *   - サブウィンドウは作らない。1 枚のパネルに座標計算で描く（装飾と同じ方針、§3.1）
 *   - 描画はサーバ側 Pixmap へ一度描いてから CopyArea する（ちらつき防止）。
 *     SPEC §9.2-4 が「ダブルバッファはタスクバーのみ」と定めた箇所。
 *   - 時計は poll のタイムアウトから分単位で更新する。タイマは持たない (§2.2.1)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * 配置
 * ------------------------------------------------------------------ */

#define TB_START_W       54    /* スタートボタンの最小幅 (§4.8)。実幅は文字幅次第 */
#define TB_START_LABEL   "スタート"
#define TB_BTN_MAX_W    160    /* タスクボタンの上限幅 */
#define TB_BTN_MIN_W     44    /* 下限。これ以下には縮めない (§4.8) */
#define TB_GAP            2
#define TB_CLOCK_W       58
#define TB_MAX_BUTTONS  128
#define TB_FLASH_MS     500    /* DEMANDS_ATTENTION の点滅周期 */
#define TB_AUTOHIDE_PX    2    /* オートハイド時に残す縁 */

struct tb_button {
	struct client *c;
	int16_t  x;
	uint16_t w;
};

static struct {
	xcb_window_t win;
	xcb_pixmap_t buf;          /* ダブルバッファ (§9.2-4) */
	uint16_t     buf_w, buf_h;
	struct rect  geom;
	bool         ready;
	bool         hidden;       /* オートハイドで引っ込んでいる */

	struct tb_button btn[TB_MAX_BUTTONS];
	uint16_t     n_btn;

	int16_t      scroll;       /* ボタンが入り切らない時の先頭位置 */
	uint8_t      hover;        /* 0=なし 1=スタート 2..=ボタン index+2 */
	uint8_t      press;
	bool         start_open;

	uint16_t     start_w;      /* スタートボタンの実幅。ラベル幅から決める */
	char         clock[8];     /* "HH:MM" */
	uint64_t     last_tick;
	bool         flash_on;
} tb;

/* スタートボタンのロゴ (旗の代わりに 1bit のマーク)。§11.1 により名称は brand.h */
static const uint8_t glyph_flag[] = {
	0x0e, 0x00,
	0x1f, 0x00,
	0x3f, 0x80,
	0x7f, 0xc0,
	0x3f, 0x80,
	0x1f, 0x00,
	0x0e, 0x00,
};
#define GLYPH_FLAG_W 10
#define GLYPH_FLAG_H 7

/* ------------------------------------------------------------------ *
 * 対象クライアントの判定 (SPEC §4.8, §5.2.2)
 * ------------------------------------------------------------------ */
static bool in_taskbar(const struct client *c)
{
	if (c->states & ST_SKIP_TASKBAR)
		return false;
	if (!type_props(c->type)->in_taskbar)
		return false;
	/* 別デスクトップのものは出さない。sticky は常に出す (§3.8) */
	if (!(c->states & ST_STICKY) && c->desktop != WM_ALL_DESKTOPS &&
	    c->desktop != wm.current_desktop)
		return false;
	return true;
}

/* ------------------------------------------------------------------ *
 * レイアウト
 *
 * ボタン幅は等分。入り切らない場合は下限まで縮め、それでも足りなければ
 * スクロールする (§4.8 の Win98 挙動)。
 * ------------------------------------------------------------------ */
static void layout(void)
{
	const struct metrics *m = theme_metrics();
	struct client *c;
	int16_t x0, avail_x, avail_w;
	uint16_t i, w;
	uint16_t trayw = tray_width();

	tb.n_btn = 0;
	for (c = wm.stack_bottom; c != NULL && tb.n_btn < TB_MAX_BUTTONS; c = c->next) {
		if (in_taskbar(c))
			tb.btn[tb.n_btn++].c = c;
	}

	x0 = (int16_t)(tb.start_w + TB_GAP * 2);
	avail_x = x0;
	avail_w = (int16_t)tb.geom.w - x0 - (int16_t)(TB_CLOCK_W * m->scale) -
	          (int16_t)trayw - TB_GAP * 2;
	if (avail_w < 0)
		avail_w = 0;

	if (tb.n_btn == 0)
		return;

	w = (uint16_t)(((uint32_t)avail_w - (uint32_t)(tb.n_btn - 1) * TB_GAP) / tb.n_btn);
	if (w > TB_BTN_MAX_W * m->scale)
		w = (uint16_t)(TB_BTN_MAX_W * m->scale);
	if (w < TB_BTN_MIN_W * m->scale)
		w = (uint16_t)(TB_BTN_MIN_W * m->scale);

	for (i = 0; i < tb.n_btn; i++) {
		tb.btn[i].x = (int16_t)(avail_x + (int32_t)i * (w + TB_GAP) - tb.scroll);
		tb.btn[i].w = w;
	}
}

/* ------------------------------------------------------------------ *
 * 描画
 * ------------------------------------------------------------------ */
static void draw_button(xcb_drawable_t d, const struct tb_button *b, uint16_t idx)
{
	const struct metrics *m = theme_metrics();
	struct client *c = b->c;
	bool active  = (wm.focused == c);
	bool pressed = active || (tb.press == idx + 2 && tb.hover == idx + 2);
	bool flash   = (c->states & ST_DEMANDS_ATTENTION) && tb.flash_on;
	int16_t y = (int16_t)(TB_GAP + 1);
	uint16_t h = (uint16_t)(tb.geom.h - 2 * (TB_GAP + 1));
	int16_t tx, ty;
	uint32_t bg, fg;

	if (b->x + (int16_t)b->w < 0 || b->x > (int16_t)tb.geom.w)
		return;                      /* 画面外 */

	bg = flash ? wm.cfg.color[THEME_HIGHLIGHT] : wm.cfg.color[THEME_FACE];
	fg = flash ? wm.cfg.color[THEME_HIGHLIGHT_TEXT] : wm.cfg.color[THEME_WINDOW_TEXT];

	draw_rect(d, b->x, y, b->w, h, bg);
	draw_bevel(d, b->x, y, b->w, h, pressed ? BEVEL_PRESSED : BEVEL_RAISED);

	/* アイコン。押し込み時は 1px 右下へ (§4.3) */
	tx = (int16_t)(b->x + 4 + (pressed ? 1 : 0));
	ty = (int16_t)(y + (int16_t)(h - m->icon_size) / 2 + (pressed ? 1 : 0));
	deco_draw_icon_at(c, d, tx, ty, m->icon_size);

	/*
	 * タイトル (§4.5.2.1)。
	 *
	 * **描画のたびに計測してはならない。** タスクバーは
	 * stack_update_client_list() 経由でフォーカス変更・レイズのたびに
	 * 全面を描き直すので、ここで font_measure_fit() を無条件に呼ぶと
	 * 窓の数だけ同期往復が発生する（20 窓・RTT 30ms なら 1 クリック 600ms）。
	 * deco.c のキャプションと同じく、幅が変わった時とタイトルが
	 * 変わった時 (icccm.c が tb_w_at_measure を 0 に落とす) にだけ測る。
	 */
	if (c->title_len > 0) {
		uint16_t avail = (uint16_t)(b->w - m->icon_size - 10);
		size_t bytes;
		int16_t bx, by;

		if (c->tb_w_at_measure != avail) {
			font_measure_fit(c->title, c->title_len, avail,
			                 &c->tb_glyphs, &c->tb_ellipsis);
			c->tb_w_at_measure = avail;
		}
		if (c->tb_glyphs == 0)
			return;

		/* tb_glyphs は UCS-2 のグリフ数。UTF-8 のバイト位置へ直す */
		{
			size_t bp = 0;
			uint8_t g = 0;
			while (bp < c->title_len && g < c->tb_glyphs) {
				unsigned char ch = (unsigned char)c->title[bp];
				bp += (ch < 0x80) ? 1 : (ch < 0xE0) ? 2 : (ch < 0xF0) ? 3 : 4;
				g++;
			}
			bytes = bp > c->title_len ? c->title_len : bp;
		}

		bx = (int16_t)(tx + m->icon_size + 3);
		by = (int16_t)(y + (int16_t)(h - font_height()) / 2
		               + (int16_t)font_ascent());
		font_draw(d, bx, by, c->title, bytes, fg);
		if (c->tb_ellipsis)
			font_draw(d, (int16_t)(bx + avail - 12), by, "...", 3, fg);
	}
}

static void draw_start(xcb_drawable_t d)
{
	const struct metrics *m = theme_metrics();
	bool pressed = tb.start_open || (tb.press == 1 && tb.hover == 1);
	int16_t x = TB_GAP, y = (int16_t)(TB_GAP + 1);
	uint16_t w = tb.start_w;
	uint16_t h = (uint16_t)(tb.geom.h - 2 * (TB_GAP + 1));
	int16_t gx, gy, tx;
	static const char label[] = TB_START_LABEL;

	(void)m;

	draw_rect(d, x, y, w, h, wm.cfg.color[THEME_FACE]);
	draw_bevel(d, x, y, w, h, pressed ? BEVEL_PRESSED : BEVEL_RAISED);

	gx = (int16_t)(x + 4 + (pressed ? 1 : 0));
	gy = (int16_t)(y + (int16_t)(h - GLYPH_FLAG_H) / 2 + (pressed ? 1 : 0));
	draw_glyph_bits(d, gx, gy, GLYPH_FLAG_W, GLYPH_FLAG_H, glyph_flag,
	                wm.cfg.color[THEME_HIGHLIGHT]);

	tx = (int16_t)(gx + GLYPH_FLAG_W + 3);
	font_draw(d, tx,
	          (int16_t)(y + (int16_t)(h - font_height()) / 2 + (int16_t)font_ascent()
	                    + (pressed ? 1 : 0)),
	          label, sizeof label - 1, wm.cfg.color[THEME_WINDOW_TEXT]);
}

static void draw_clock(xcb_drawable_t d)
{
	const struct metrics *m = theme_metrics();
	uint16_t w = (uint16_t)(TB_CLOCK_W * m->scale);
	int16_t x = (int16_t)(tb.geom.w - w - TB_GAP);
	int16_t y = (int16_t)(TB_GAP + 1);
	uint16_t h = (uint16_t)(tb.geom.h - 2 * (TB_GAP + 1));
	uint16_t tw;

	draw_rect(d, x, y, w, h, wm.cfg.color[THEME_FACE]);
	draw_bevel(d, x, y, w, h, BEVEL_SUNKEN);

	tw = font_text_width(tb.clock, strlen(tb.clock));
	font_draw(d, (int16_t)(x + (int16_t)(w - tw) / 2),
	          (int16_t)(y + (int16_t)(h - font_height()) / 2 + (int16_t)font_ascent()),
	          tb.clock, strlen(tb.clock), wm.cfg.color[THEME_WINDOW_TEXT]);
}

void taskbar_draw(const xcb_rectangle_t *clip)
{
	uint16_t i;

	(void)clip;   /* パネルはバッファへ全面描いて 1 回 CopyArea する */
	if (!tb.ready || tb.win == XCB_WINDOW_NONE)
		return;

	/* 背景と上端の 1px ハイライト (§4.8) */
	draw_rect(tb.buf, 0, 0, tb.geom.w, tb.geom.h, wm.cfg.color[THEME_FACE]);
	draw_line(tb.buf, 0, 0, (int16_t)(tb.geom.w - 1), 0,
	          wm.cfg.color[THEME_HILIGHT]);

	draw_start(tb.buf);
	for (i = 0; i < tb.n_btn; i++)
		draw_button(tb.buf, &tb.btn[i], i);
	draw_clock(tb.buf);

	{
		xcb_gcontext_t gc = xcb_generate_id(wm.conn);
		uint32_t v = 0;
		xcb_create_gc(wm.conn, gc, tb.win, XCB_GC_GRAPHICS_EXPOSURES, &v);
		xcb_copy_area(wm.conn, tb.buf, tb.win, gc, 0, 0, 0, 0,
		              tb.geom.w, tb.geom.h);
		xcb_free_gc(wm.conn, gc);
	}
	tray_draw();
}

/* ------------------------------------------------------------------ *
 * strut (SPEC §3.5.2)
 *
 * 自分の分だけ設定し、作業領域の計算そのものは layout.c に任せる。
 * ここで作業領域を直接いじると二重計上になる。
 * ------------------------------------------------------------------ */
void taskbar_update_strut(void)
{
	uint32_t sp[12];
	uint32_t s4[4];

	if (!tb.ready)
		return;

	memset(sp, 0, sizeof sp);
	memset(s4, 0, sizeof s4);

	if (!tb.hidden) {
		/* 下端に置く場合のみ実装（上下左右は設定で切替、§8） */
		sp[3]  = tb.geom.h;                    /* bottom */
		sp[10] = (uint32_t)tb.geom.x;          /* bottom_start_x */
		sp[11] = (uint32_t)(tb.geom.x + tb.geom.w - 1);
		s4[3]  = tb.geom.h;
	}
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tb.win,
		atoms[ATOM_NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, 32, 12, sp);
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tb.win,
		atoms[ATOM_NET_WM_STRUT], XCB_ATOM_CARDINAL, 32, 4, s4);

	layout_update_workareas();
}

/* ------------------------------------------------------------------ *
 * 生成・破棄
 * ------------------------------------------------------------------ */
void taskbar_init(void)
{
	const struct metrics *m = theme_metrics();
	struct monitor *mon;
	uint32_t vals[3];
	xcb_atom_t type;

	if (!wm.cfg.taskbar)
		return;

	mon = &wm.monitors[0];
	tb.geom.x = mon->geom.x;
	tb.geom.h = m->taskbar_h;
	tb.geom.w = mon->geom.w;
	tb.geom.y = (int16_t)(mon->geom.y + mon->geom.h - tb.geom.h);

	tb.win = xcb_generate_id(wm.conn);
	/* 値の並びはビット値の昇順: BACK_PIXEL(2) < OVERRIDE_REDIRECT(512) < EVENT_MASK(2048) */
	vals[0] = wm.cfg.color[THEME_FACE];
	vals[1] = 1;
	vals[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
	          XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
	          XCB_EVENT_MASK_LEAVE_WINDOW;
	xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, tb.win, wm.root,
	                  tb.geom.x, tb.geom.y, tb.geom.w, tb.geom.h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
	                  XCB_CW_EVENT_MASK, vals);

	/*
	 * 自分自身を DOCK として宣言する (§5.2.2)。
	 * override_redirect なので w98wm 自身の管理下には入らないが、
	 * pager や他のツールが正しく扱えるようにプロパティは立てておく。
	 */
	type = atoms[ATOM_NET_WM_WINDOW_TYPE_DOCK];
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tb.win,
		atoms[ATOM_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &type);
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tb.win,
		atoms[ATOM_NET_WM_NAME], atoms[ATOM_UTF8_STRING], 8,
		(uint32_t)strlen(WM_DISPLAY_NAME " taskbar"), WM_DISPLAY_NAME " taskbar");

	/*
	 * スタートボタンの幅はラベルの実測から決める (§4.8)。
	 * Win98 の 54px は英字前提の値で、日本語ラベル (13px×4 の全角) では
	 * 文字が切れる。ここで一度だけ測る（描画経路では測らない。§4.5.2.1）。
	 */
	{
		uint16_t lw = font_text_width(TB_START_LABEL, sizeof TB_START_LABEL - 1);
		uint16_t need = (uint16_t)(4 + GLYPH_FLAG_W + 3 + lw + 8);
		uint16_t min_w = (uint16_t)(TB_START_W * m->scale);
		tb.start_w = need > min_w ? need : min_w;
	}

	tb.buf = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, wm.depth, tb.buf, wm.root, tb.geom.w, tb.geom.h);
	tb.buf_w = tb.geom.w;
	tb.buf_h = tb.geom.h;

	xcb_map_window(wm.conn, tb.win);
	tb.ready = true;

	taskbar_update_strut();
	taskbar_tick(wm_now_ms());
	taskbar_update();
	LOG("タスクバーを作成 (%dx%d+%d+%d)",
	    tb.geom.w, tb.geom.h, tb.geom.x, tb.geom.y);
}

void taskbar_fini(void)
{
	if (!tb.ready)
		return;
	if (tb.buf != XCB_PIXMAP_NONE)
		xcb_free_pixmap(wm.conn, tb.buf);
	if (tb.win != XCB_WINDOW_NONE)
		xcb_destroy_window(wm.conn, tb.win);
	tb.ready = false;
}

xcb_window_t taskbar_strut_window(void)
{
	return tb.ready ? tb.win : XCB_WINDOW_NONE;
}

bool taskbar_owns(xcb_window_t w)
{
	return tb.ready && w == tb.win;
}

void taskbar_update(void)
{
	uint16_t trayw;

	if (!tb.ready)
		return;
	layout();

	trayw = tray_width();
	tray_place((int16_t)(tb.geom.w - TB_CLOCK_W * theme_metrics()->scale
	                     - TB_GAP * 2 - trayw),
	           (int16_t)(TB_GAP + 1),
	           (uint16_t)(tb.geom.h - 2 * (TB_GAP + 1)));

	taskbar_draw(NULL);
}

/* ------------------------------------------------------------------ *
 * 時計と点滅 (SPEC §4.8)
 * ------------------------------------------------------------------ */
void taskbar_tick(uint64_t now_ms)
{
	time_t t;
	struct tm tmv;
	char buf[8];
	bool redraw = false;
	uint16_t i;

	if (!tb.ready)
		return;

	t = time(NULL);
	if (localtime_r(&t, &tmv) != NULL) {
		snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
		if (strcmp(buf, tb.clock) != 0) {
			memcpy(tb.clock, buf, sizeof tb.clock);
			redraw = true;
		}
	}

	/* DEMANDS_ATTENTION の点滅。対象が無ければタイマを回さない */
	for (i = 0; i < tb.n_btn; i++) {
		if (tb.btn[i].c->states & ST_DEMANDS_ATTENTION) {
			if (now_ms - tb.last_tick >= TB_FLASH_MS) {
				tb.flash_on = !tb.flash_on;
				tb.last_tick = now_ms;
				redraw = true;
			}
			break;
		}
	}

	if (redraw)
		taskbar_draw(NULL);
}

/* ------------------------------------------------------------------ *
 * 入力
 * ------------------------------------------------------------------ */
static uint8_t hit_test(int16_t x, int16_t y)
{
	const struct metrics *m = theme_metrics();
	uint16_t i;

	(void)y;
	(void)m;
	if (x < (int16_t)(tb.start_w + TB_GAP))
		return 1;                              /* スタートボタン */
	for (i = 0; i < tb.n_btn; i++) {
		if (x >= tb.btn[i].x && x < tb.btn[i].x + (int16_t)tb.btn[i].w)
			return (uint8_t)(i + 2);
	}
	return 0;
}

static void activate_button(uint16_t idx)
{
	struct client *c;

	if (idx >= tb.n_btn)
		return;
	c = tb.btn[idx].c;

	/*
	 * Win98 の挙動: アクティブなボタンを押すと最小化、
	 * それ以外なら復元してアクティブにする。
	 */
	if (wm.focused == c && !(c->flags & CF_ICONIC)) {
		client_iconify(c);
	} else {
		if (c->flags & CF_ICONIC)
			client_deiconify(c);
		ewmh_set_demands_attention(c, false);
		stack_raise(c);
		stack_apply();
		focus_set(c, wm.last_time);
	}
	taskbar_update();
}

bool taskbar_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & 0x7f;

	if (!tb.ready)
		return false;

	switch (type) {
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *)ev;
		if (e->window != tb.win)
			return false;
		if (e->count == 0)
			taskbar_draw(NULL);
		return true;
	}
	case XCB_BUTTON_PRESS: {
		xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
		if (e->event != tb.win)
			return false;
		tb.press = hit_test(e->event_x, e->event_y);
		tb.hover = tb.press;
		taskbar_draw(NULL);
		return true;
	}
	case XCB_BUTTON_RELEASE: {
		xcb_button_release_event_t *e = (xcb_button_release_event_t *)ev;
		uint8_t part;
		if (e->event != tb.win)
			return false;
		part = hit_test(e->event_x, e->event_y);
		if (part == tb.press && part != 0) {
			if (part == 1) {
				/*
				 * スタートメニュー。menu.c のポップアップを再利用 (§4.9)。
				 * 閉じる側は menu.c が taskbar_start_closed() で
				 * 知らせてくるので、ここでは開くだけにする
				 * （両方でトグルすると状態が二重になる）。
				 */
				tb.start_open = true;
				menu_open_start(tb.geom.x + TB_GAP, tb.geom.y);
			} else {
				activate_button((uint16_t)(part - 2));
			}
		}
		tb.press = 0;
		taskbar_draw(NULL);
		return true;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;
		uint8_t part;
		if (e->event != tb.win)
			return false;
		part = hit_test(e->event_x, e->event_y);
		if (part != tb.hover) {
			tb.hover = part;
			if (tb.press != 0)
				taskbar_draw(NULL);
		}
		return true;
	}
	case XCB_LEAVE_NOTIFY: {
		xcb_leave_notify_event_t *e = (xcb_leave_notify_event_t *)ev;
		if (e->event != tb.win)
			return false;
		if (tb.hover != 0) {
			tb.hover = 0;
			if (tb.press != 0)
				taskbar_draw(NULL);
		}
		return true;
	}
	default:
		break;
	}
	return false;
}

/*
 * キーバインド (Ctrl+Esc / Super) から開く (§6)。
 * タスクバーが無効なら画面左下を基準にして、それでも開けるようにする。
 */
void taskbar_open_start_menu(void)
{
	int16_t x, bottom;

	if (menu_active()) {
		menu_close();
		return;
	}

	if (tb.ready) {
		x = (int16_t)(tb.geom.x + TB_GAP);
		bottom = tb.geom.y;
		tb.start_open = true;
		taskbar_draw(NULL);
	} else {
		x = 0;
		bottom = wm.screen ? (int16_t)wm.screen->height_in_pixels : 768;
	}
	menu_open_start(x, bottom);
}

/* スタートメニューが閉じたことをタスクバーへ知らせる (menu.c から呼ぶ) */
void taskbar_start_closed(void)
{
	if (tb.start_open) {
		tb.start_open = false;
		taskbar_draw(NULL);
	}
}
