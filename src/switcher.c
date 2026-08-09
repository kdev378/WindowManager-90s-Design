/*
 * switcher.c - Alt+Tab のタスクスイッチャ (SPEC §6)
 *
 * Win98 の「タスクの切り替え」パネル: 画面中央に浮かぶ 1 枚の
 * override-redirect ウィンドウに、対象ウィンドウのアイコンを格子状に並べ、
 * 選択中の 1 つを枠で囲み、下段にそのタイトルを出す。
 *
 * 実装上の要点:
 *
 *  - **候補は MRU 順** (wm.focus_list)。開いた瞬間の並びで固定する。
 *    途中で focus_set() を呼んで MRU を掻き回すと、Tab を押すたびに
 *    2 つの窓を往復するだけになる（Win98 も確定時にしか順序を変えない）。
 *
 *  - **キーボードを能動 grab する**。パッシブ grab（キーバインド）の
 *    発火中に GrabKeyboard するのは、同じクライアントが所有している
 *    限り AlreadyGrabbed にならない。grab に失敗したら**パネルを出さず**、
 *    MRU の次点へ直接フォーカスを移す縮退動作に落ちる
 *    （grab できないのに入力を飲み込むと操作不能になる。§4.6 と同じ方針）。
 *
 *  - **終了は修飾キーの KeyRelease で判定する**。どの修飾で起動したかは
 *    input.c から渡ってこないので、Alt/Meta/Super/Control のいずれかが
 *    離されたら確定する。Win98 の Alt+Tab / Alt+Esc 双方でこれが正しい。
 *
 *  - 計測 (font_text_width) は選択が変わった時にだけ行い、描画経路
 *    (draw_panel) では一切行わない (§4.5.2.1)。
 */
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

#define XK_MISCELLANY 1
#define XK_XKB_KEYS   1
#include <X11/keysymdef.h>

#include <xcb/xcb_keysyms.h>

#define SW_MAX          64   /* これを超える窓数は切り捨てる */
#define SW_PAD           8   /* パネル外周の余白 */
#define SW_CELL_PAD      8   /* アイコンの周囲 */
#define SW_MAX_COLS      8   /* Win98 と同じく 1 行 8 個で折り返す */
#define SW_TEXT_PAD      4

static struct {
	bool          active;
	xcb_window_t  win;
	xcb_key_symbols_t *keysyms;

	struct client *list[SW_MAX];
	uint16_t      n;
	uint16_t      sel;

	uint16_t      cols, rows;
	uint16_t      cell;          /* 正方セルの一辺 */
	int16_t       grid_x;        /* 格子の左端。パネルが広い時は中央寄せ */
	int16_t       x, y;
	uint16_t      w, h;

	/* 選択中タイトルの描画キャッシュ (§4.5.2.1)。計測は select() でのみ */
	uint8_t       title_glyphs;
	uint8_t       title_ellipsis;
	uint16_t      title_w;
} sw;

/* ------------------------------------------------------------------ *
 * 候補の収集
 * ------------------------------------------------------------------ */

/*
 * Alt+Tab に出す条件。タスクバーとほぼ同じだが、
 * **最小化された窓も対象に含める**（Win98 はそうする）。
 */
static bool switchable(const struct client *c)
{
	if (c->states & ST_SKIP_TASKBAR)
		return false;
	if (!type_props(c->type)->in_taskbar)
		return false;
	if (!type_props(c->type)->focusable)
		return false;
	if (!(c->states & ST_STICKY) && c->desktop != WM_ALL_DESKTOPS &&
	    c->desktop != wm.current_desktop)
		return false;
	return true;
}

static void collect(void)
{
	struct client *c;

	sw.n = 0;
	for (c = wm.focus_list; c != NULL && sw.n < SW_MAX; c = c->focus_next)
		if (switchable(c))
			sw.list[sw.n++] = c;
}

/* 破棄されたクライアントを候補から外す。sel は詰めた後の位置へ寄せる */
static void drop(uint16_t idx)
{
	if (idx >= sw.n)
		return;
	memmove(&sw.list[idx], &sw.list[idx + 1],
	        (size_t)(sw.n - idx - 1) * sizeof sw.list[0]);
	sw.n--;
	if (sw.sel >= sw.n)
		sw.sel = sw.n > 0 ? (uint16_t)(sw.n - 1) : 0;
}

/* ------------------------------------------------------------------ *
 * レイアウト
 * ------------------------------------------------------------------ */
static void compute_layout(void)
{
	const struct metrics *m = theme_metrics();
	uint16_t cols;

	sw.cell = (uint16_t)(m->icon_size + 2 * SW_CELL_PAD);

	cols = sw.n < SW_MAX_COLS ? sw.n : SW_MAX_COLS;
	if (cols == 0)
		cols = 1;
	sw.cols = cols;
	sw.rows = (uint16_t)((sw.n + cols - 1) / cols);
	if (sw.rows == 0)
		sw.rows = 1;

	sw.w = (uint16_t)(2 * SW_PAD + sw.cols * sw.cell);
	sw.h = (uint16_t)(2 * SW_PAD + sw.rows * sw.cell +
	                  SW_TEXT_PAD + font_height() + SW_TEXT_PAD);

	/* タイトル 1 行分の最低幅。狭すぎると 2〜3 文字しか出ない */
	if (sw.w < 260)
		sw.w = 260;

	sw.grid_x = (int16_t)(((int)sw.w - (int)sw.cols * (int)sw.cell) / 2);
}

static void center_on_monitor(void)
{
	struct monitor *mon = NULL;
	struct rect g;

	if (wm.focused != NULL)
		mon = layout_monitor_at(wm.focused->geom.x, wm.focused->geom.y);
	if (mon == NULL && wm.n_monitors > 0)
		mon = &wm.monitors[0];

	if (mon != NULL) {
		g = mon->geom;
	} else {
		g.x = 0;
		g.y = 0;
		g.w = wm.screen ? (uint16_t)wm.screen->width_in_pixels  : 1024u;
		g.h = wm.screen ? (uint16_t)wm.screen->height_in_pixels : 768u;
	}

	sw.x = (int16_t)(g.x + ((int)g.w - (int)sw.w) / 2);
	sw.y = (int16_t)(g.y + ((int)g.h - (int)sw.h) / 2);
	if (sw.x < g.x)
		sw.x = g.x;
	if (sw.y < g.y)
		sw.y = g.y;
}

/* UCS-2 グリフ数 n に対応する UTF-8 のバイト位置を返す (§4.5.4.1 の注意) */
static size_t bytes_for_glyphs(const char *utf8, size_t len, uint8_t glyphs)
{
	size_t b = 0;
	uint8_t g = 0;

	while (b < len && g < glyphs) {
		unsigned char ch = (unsigned char)utf8[b];
		b += (ch < 0x80) ? 1 : (ch < 0xE0) ? 2 : (ch < 0xF0) ? 3 : 4;
		g++;
	}
	return b > len ? len : b;
}

/* 選択が変わった時にだけタイトルを測る。描画経路からは呼ばない (§4.5.2.1) */
static void measure_title(void)
{
	struct client *c;
	uint16_t avail;

	sw.title_glyphs = 0;
	sw.title_ellipsis = 0;
	sw.title_w = 0;

	if (sw.sel >= sw.n)
		return;
	c = sw.list[sw.sel];
	if (c->title_len == 0)
		return;

	avail = (uint16_t)(sw.w - 2 * SW_PAD);
	font_measure_fit(c->title, c->title_len, avail,
	                 &sw.title_glyphs, &sw.title_ellipsis);

	/* 中央寄せの x と省略記号の位置は描画時に測らず、ここで一度だけ求める */
	if (sw.title_glyphs > 0) {
		size_t b = bytes_for_glyphs(c->title, c->title_len, sw.title_glyphs);
		sw.title_w = font_text_width(c->title, b);
	}
}

/* ------------------------------------------------------------------ *
 * 描画
 * ------------------------------------------------------------------ */
static void draw_panel(void)
{
	const struct metrics *m = theme_metrics();
	uint16_t i;
	int text_y;

	if (!sw.active || sw.win == XCB_WINDOW_NONE)
		return;

	draw_rect(sw.win, 0, 0, sw.w, sw.h, wm.cfg.color[THEME_FACE]);
	draw_bevel(sw.win, 0, 0, sw.w, sw.h, BEVEL_RAISED);

	for (i = 0; i < sw.n; i++) {
		int col = i % sw.cols;
		int row = i / sw.cols;
		int cx = sw.grid_x + col * (int)sw.cell;
		int cy = SW_PAD + row * (int)sw.cell;

		if (i == sw.sel) {
			/* Win98 の選択枠。セルの外周に沈んだベベルを引く */
			draw_bevel(sw.win, (int16_t)cx, (int16_t)cy,
			           sw.cell, sw.cell, BEVEL_SUNKEN);
		}
		deco_draw_icon_at(sw.list[i], sw.win,
		                  (int16_t)(cx + SW_CELL_PAD),
		                  (int16_t)(cy + SW_CELL_PAD),
		                  m->icon_size);
	}

	text_y = SW_PAD + (int)sw.rows * (int)sw.cell + SW_TEXT_PAD;

	if (sw.sel < sw.n && sw.title_glyphs > 0) {
		struct client *c = sw.list[sw.sel];
		size_t bytes = bytes_for_glyphs(c->title, c->title_len, sw.title_glyphs);
		int tx = ((int)sw.w - (int)sw.title_w) / 2;

		if (tx < SW_PAD)
			tx = SW_PAD;
		font_draw(sw.win, (int16_t)tx,
		          (int16_t)(text_y + (int)font_ascent()),
		          c->title, bytes, wm.cfg.color[THEME_WINDOW_TEXT]);
		if (sw.title_ellipsis)
			font_draw(sw.win,
			          (int16_t)(tx + (int)sw.title_w),
			          (int16_t)(text_y + (int)font_ascent()),
			          "...", 3, wm.cfg.color[THEME_WINDOW_TEXT]);
	}
}

/* ------------------------------------------------------------------ *
 * 選択の移動
 * ------------------------------------------------------------------ */
static void select_index(uint16_t idx)
{
	if (sw.n == 0)
		return;
	sw.sel = (uint16_t)(idx % sw.n);
	measure_title();
	draw_panel();
	xcb_flush(wm.conn);
}

/* ------------------------------------------------------------------ *
 * grab
 * ------------------------------------------------------------------ */
static bool grab_keyboard(void)
{
	xcb_grab_keyboard_cookie_t kc;
	xcb_grab_keyboard_reply_t *kr;
	bool ok;

	kc = xcb_grab_keyboard(wm.conn, 0, wm.root, XCB_CURRENT_TIME,
	                       XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	kr = xcb_grab_keyboard_reply(wm.conn, kc, NULL);
	ok = kr != NULL && kr->status == XCB_GRAB_STATUS_SUCCESS;
	free(kr);
	return ok;
}

/* grab できなかった時の縮退動作: MRU の隣へ直接移る（パネルは出さない） */
static void fallback_step(bool backwards)
{
	collect();
	if (sw.n < 2)
		return;
	/* list[0] は現在フォーカス中の窓。次点/末尾へ移る */
	focus_set(backwards ? sw.list[sw.n - 1] : sw.list[1], wm.last_time);
	sw.n = 0;
}

/* ------------------------------------------------------------------ *
 * 公開 API
 * ------------------------------------------------------------------ */
bool switcher_active(void)
{
	return sw.active;
}

void switcher_drop_client(struct client *c)
{
	uint16_t i;

	if (!sw.active || c == NULL)
		return;

	for (i = 0; i < sw.n; i++) {
		if (sw.list[i] == c) {
			drop(i);
			break;
		}
	}
	if (sw.n < 2) {
		switcher_end(true);   /* 相手がいなくなった。黙って畳む */
		return;
	}
	select_index(sw.sel);
}

void switcher_begin(bool backwards)
{
	uint32_t vals[3];

	if (sw.active) {
		switcher_step(backwards);
		return;
	}

	collect();
	if (sw.n < 2) {
		sw.n = 0;
		return;                       /* 切り替える相手がいない */
	}

	if (!grab_keyboard()) {
		ERR("スイッチャ: キーボードを grab できなかった。縮退動作に落ちる");
		fallback_step(backwards);
		return;
	}

	compute_layout();
	center_on_monitor();

	sw.win = xcb_generate_id(wm.conn);
	/* 値はビット値の昇順: BACK_PIXEL(2) < OVERRIDE_REDIRECT(512) < EVENT_MASK(2048) */
	vals[0] = wm.cfg.color[THEME_FACE];
	vals[1] = 1;
	vals[2] = XCB_EVENT_MASK_EXPOSURE;
	xcb_create_window(wm.conn,
	                  wm.depth != 0 ? wm.depth : (uint8_t)XCB_COPY_FROM_PARENT,
	                  sw.win, wm.root, sw.x, sw.y, sw.w, sw.h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
	                  XCB_CW_EVENT_MASK, vals);
	xcb_map_window(wm.conn, sw.win);

	sw.keysyms = xcb_key_symbols_alloc(wm.conn);
	sw.active = true;

	/* list[0] は今フォーカスしている窓。開いた時点で既に 1 つ進める */
	sw.sel = 0;
	select_index(backwards ? (uint16_t)(sw.n - 1) : 1);
}

void switcher_step(bool backwards)
{
	if (!sw.active || sw.n == 0)
		return;
	select_index((uint16_t)((sw.sel + (backwards ? sw.n - 1 : 1)) % sw.n));
}

void switcher_end(bool cancel)
{
	struct client *target = NULL;

	if (!sw.active)
		return;

	if (!cancel && sw.sel < sw.n)
		target = sw.list[sw.sel];

	xcb_ungrab_keyboard(wm.conn, XCB_CURRENT_TIME);

	if (sw.keysyms != NULL) {
		xcb_key_symbols_free(sw.keysyms);
		sw.keysyms = NULL;
	}
	if (sw.win != XCB_WINDOW_NONE) {
		xcb_destroy_window(wm.conn, sw.win);
		sw.win = XCB_WINDOW_NONE;
	}

	sw.active = false;
	sw.n = 0;
	sw.sel = 0;

	if (target != NULL) {
		if (target->flags & CF_ICONIC)
			client_deiconify(target);
		ewmh_set_demands_attention(target, false);
		stack_raise(target);
		stack_apply();
		focus_set(target, wm.last_time);
		taskbar_update();
	}

	xcb_flush(wm.conn);
}

/* ------------------------------------------------------------------ *
 * イベント
 * ------------------------------------------------------------------ */

/* 起動に使われた修飾キーが離されたか。どの修飾かは分からないので総当たり */
static bool is_end_modifier(xcb_keysym_t sym)
{
	switch (sym) {
	case XK_Alt_L:     case XK_Alt_R:
	case XK_Meta_L:    case XK_Meta_R:
	case XK_Super_L:   case XK_Super_R:
	case XK_Hyper_L:   case XK_Hyper_R:
	case XK_Control_L: case XK_Control_R:
		return true;
	default:
		return false;
	}
}

bool switcher_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & 0x7f;

	if (!sw.active)
		return false;

	switch (type) {
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *)ev;

		if (e->window != sw.win)
			return false;
		if (e->count == 0)
			draw_panel();
		return true;
	}
	case XCB_KEY_PRESS: {
		xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
		xcb_keysym_t sym;

		if (sw.keysyms == NULL)
			return true;
		/* 列 0 = 修飾なしの keysym。Shift+Tab でも Tab が返る */
		sym = xcb_key_symbols_get_keysym(sw.keysyms, e->detail, 0);

		switch (sym) {
		case XK_Escape:
			switcher_end(true);
			return true;
		case XK_Return:
		case XK_KP_Enter:
			switcher_end(false);
			return true;
		case XK_Tab:
		case XK_ISO_Left_Tab:
			switcher_step((e->state & XCB_MOD_MASK_SHIFT) != 0);
			return true;
		case XK_Right:
		case XK_Down:
			switcher_step(false);
			return true;
		case XK_Left:
		case XK_Up:
			switcher_step(true);
			return true;
		default:
			return true;      /* パネル表示中は他のキーを飲み込む */
		}
	}
	case XCB_KEY_RELEASE: {
		xcb_key_release_event_t *e = (xcb_key_release_event_t *)ev;
		xcb_keysym_t sym;

		if (sw.keysyms == NULL)
			return true;
		sym = xcb_key_symbols_get_keysym(sw.keysyms, e->detail, 0);
		if (is_end_modifier(sym))
			switcher_end(false);
		return true;
	}
	case XCB_BUTTON_PRESS:
		/* パネル外のクリックは取り消し扱い（ポインタは grab していない） */
		switcher_end(true);
		return false;     /* 元の処理にも流す。クリック先が普通に反応してよい */
	/*
	 * DestroyNotify / UnmapNotify はここで処理しない。候補から外すのは
	 * client_unmanage() が呼ぶ switcher_drop_client() の役目である
	 * （withdraw は DestroyNotify を伴わないので、ここで見ていると漏れる）。
	 */
	default:
		break;
	}
	return false;
}
