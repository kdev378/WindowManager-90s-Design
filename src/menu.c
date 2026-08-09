/*
 * menu.c - Win98 風ポップアップメニュー (SPEC §4.6)
 *
 * ここではポップアップの「入れ物」（生成・配置・grab・入力処理・描画）を
 * 汎用に保ち、ウィンドウメニューの項目内容だけをこのファイルの下半分に置く。
 * Phase 4 のスタートメニューも同じ入れ物を再利用する想定 (w98wm.h の
 * コメントどおり)。
 *
 * メニューは「滅多に開かない」ため、開くたびに 1 枚の override-redirect
 * ウィンドウを作り、閉じるときに破棄する（常駐させない）。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

#define XK_MISCELLANY 1
#include <X11/keysymdef.h>

#include <xcb/xcb_keysyms.h>

/* ================================================================== *
 * 定数
 * ================================================================== */

#define MENU_MAX_ITEMS  32
#define MENU_BORDER      2   /* draw_bevel(BEVEL_RAISED) が外側+内側で 2px 引く */
#define CONTENT_PAD      3   /* 縁と項目列の間の余白 */
#define TEXT_LEFT_PAD    20  /* Win98 のチェック/アイコン用ガター相当 */
#define TEXT_RIGHT_PAD   8
#define ACCEL_GAP        16  /* ラベルとアクセラレータ文字列の最小間隔 */
#define MENU_MIN_CONTENT_W 96
#define BANNER_W         22  /* スタートメニュー左端の縦バナー (§4.9) */
#define BANNER_TEXT_GAP   2

/* ================================================================== *
 * 状態
 * ================================================================== */

struct menu_state {
	bool            active;
	xcb_window_t    win;
	struct client  *client;          /* menu_close() 前に action へ渡す */
	xcb_key_symbols_t *keysyms;

	struct menu_item items[MENU_MAX_ITEMS];
	int             n_items;
	int             item_top[MENU_MAX_ITEMS];  /* window 内の絶対 y */
	int             item_h[MENU_MAX_ITEMS];
	uint16_t        accel_w[MENU_MAX_ITEMS];   /* compute_layout() でのみ計測 (§4.5.2) */

	int             selected;        /* 選択中の index。無ければ -1 */

	bool            is_start;        /* スタートメニュー (§4.9) */
	uint16_t        banner_w;        /* 縦バナーの幅。0 なら無し */

	int16_t         x, y;
	uint16_t        w, h;
};

static struct menu_state g_menu;

/* ================================================================== *
 * 汎用ポップアップ機構
 * ================================================================== */

/* 項目列の左端 x と幅。バナーの分だけ右へずれる (§4.9) */
static int content_x(void)
{
	return MENU_BORDER + (int)g_menu.banner_w;
}

static int content_w(void)
{
	int w = (int)g_menu.w - 2 * MENU_BORDER - (int)g_menu.banner_w;
	return w < 0 ? 0 : w;
}

static bool item_selectable(int i)
{
	if (i < 0 || i >= g_menu.n_items)
		return false;
	return !g_menu.items[i].separator && g_menu.items[i].enabled;
}

static int first_selectable(void)
{
	int i;

	for (i = 0; i < g_menu.n_items; i++)
		if (item_selectable(i))
			return i;
	return -1;
}

static int last_selectable(void)
{
	int i;

	for (i = g_menu.n_items - 1; i >= 0; i--)
		if (item_selectable(i))
			return i;
	return -1;
}

/* dir = +1 (下) / -1 (上)。セパレータと無効項目は飛ばす。1 周して無ければ -1 */
static int next_selectable(int from, int dir)
{
	int i, guard;

	if (g_menu.n_items <= 0)
		return -1;

	i = from;
	for (guard = 0; guard < g_menu.n_items; guard++) {
		i += dir;
		if (i < 0)
			i = g_menu.n_items - 1;
		else if (i >= g_menu.n_items)
			i = 0;
		if (item_selectable(i))
			return i;
	}
	return -1;
}

/* ラベル末尾の "(X)" からアクセラレータ文字を取り出す。無ければ 0 */
static char item_accel_letter(const struct menu_item *it)
{
	const char *p;

	if (!it->label)
		return 0;
	p = strrchr(it->label, '(');
	if (!p)
		return 0;
	p++;
	if (*p != '\0' && p[1] == ')')
		return (char)toupper((unsigned char)*p);
	return 0;
}

/*
 * 幅と各項目の位置を決める。SPEC §4.5.2「font_text_width は描画経路から
 * 呼んではならない」ので、ここ（メニューを開いた直後の 1 回だけ）で測って
 * accel_w[] へキャッシュし、draw_menu() 側は一切計測しない。
 */
static void compute_layout(void)
{
	const struct metrics *m = theme_metrics();
	int y = MENU_BORDER + CONTENT_PAD;
	int content_width = MENU_MIN_CONTENT_W;
	int i;

	for (i = 0; i < g_menu.n_items; i++) {
		struct menu_item *it = &g_menu.items[i];
		int h;

		g_menu.item_top[i] = y;
		g_menu.accel_w[i] = 0;

		if (it->separator) {
			h = (int)m->menu_sep_h;
		} else {
			uint16_t label_w = font_text_width(it->label, strlen(it->label));
			int need = TEXT_LEFT_PAD + (int)label_w + TEXT_RIGHT_PAD;

			if (it->accel) {
				g_menu.accel_w[i] = font_text_width(it->accel, strlen(it->accel));
				need += ACCEL_GAP + (int)g_menu.accel_w[i];
			}
			if (need > content_width)
				content_width = need;
			h = (int)m->menu_item_h;
		}

		g_menu.item_h[i] = h;
		y += h;
	}

	g_menu.h = (uint16_t)(y + CONTENT_PAD + MENU_BORDER);
	g_menu.w = (uint16_t)(content_width + 2 * MENU_BORDER + (int)g_menu.banner_w);
}

/* 要求座標をモニタの作業領域内へ収める（右/下にはみ出せば反転） */
static void position_popup(int16_t root_x, int16_t root_y)
{
	struct monitor *mon = layout_monitor_at(root_x, root_y);
	struct rect wa;
	int16_t x, y;

	if (mon) {
		wa = mon->workarea;
	} else {
		wa.x = 0;
		wa.y = 0;
		wa.w = wm.screen ? (uint16_t)wm.screen->width_in_pixels  : 1024u;
		wa.h = wm.screen ? (uint16_t)wm.screen->height_in_pixels : 768u;
	}

	x = root_x;
	y = root_y;

	if ((int)x + (int)g_menu.w > (int)wa.x + (int)wa.w)
		x = (int16_t)(root_x - (int16_t)g_menu.w);
	if ((int)y + (int)g_menu.h > (int)wa.y + (int)wa.h)
		y = (int16_t)(root_y - (int16_t)g_menu.h);

	if ((int)x + (int)g_menu.w > (int)wa.x + (int)wa.w)
		x = (int16_t)((int)wa.x + (int)wa.w - (int)g_menu.w);
	if ((int)y + (int)g_menu.h > (int)wa.y + (int)wa.h)
		y = (int16_t)((int)wa.y + (int)wa.h - (int)g_menu.h);
	if (x < wa.x)
		x = wa.x;
	if (y < wa.y)
		y = wa.y;

	g_menu.x = x;
	g_menu.y = y;
}

static void create_popup_window(void)
{
	uint32_t vals[3];

	g_menu.win = xcb_generate_id(wm.conn);
	vals[0] = wm.cfg.color[THEME_MENU_BG];
	vals[1] = 1;   /* override_redirect: SubstructureRedirect を経由させない */
	vals[2] = XCB_EVENT_MASK_EXPOSURE;

	xcb_create_window(wm.conn,
	                  wm.depth != 0 ? wm.depth : (uint8_t)XCB_COPY_FROM_PARENT,
	                  g_menu.win, wm.root,
	                  g_menu.x, g_menu.y, g_menu.w, g_menu.h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
	                  vals);
	xcb_map_window(wm.conn, g_menu.win);
}

/*
 * ポインタとキーボードを grab する (SPEC §4.6)。
 * どちらか一方でも失敗したら即座に諦める。中途半端に grab したまま
 * メニューを残すと入力が詰まる ("grab できないメニューはイベントを
 * 飲み込んではならない" という要求どおり)。
 */
static bool grab_input(void)
{
	xcb_grab_pointer_cookie_t pc;
	xcb_grab_pointer_reply_t *pr;
	xcb_grab_keyboard_cookie_t kc;
	xcb_grab_keyboard_reply_t *kr;
	bool ptr_ok, kb_ok;

	pc = xcb_grab_pointer(wm.conn, 0, wm.root,
	                      XCB_EVENT_MASK_BUTTON_PRESS |
	                      XCB_EVENT_MASK_BUTTON_RELEASE |
	                      XCB_EVENT_MASK_POINTER_MOTION,
	                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
	                      XCB_NONE, cursor_get(CURSOR_ARROW), XCB_CURRENT_TIME);
	pr = xcb_grab_pointer_reply(wm.conn, pc, NULL);
	ptr_ok = pr != NULL && pr->status == XCB_GRAB_STATUS_SUCCESS;
	free(pr);
	if (!ptr_ok)
		return false;

	kc = xcb_grab_keyboard(wm.conn, 0, wm.root, XCB_CURRENT_TIME,
	                       XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	kr = xcb_grab_keyboard_reply(wm.conn, kc, NULL);
	kb_ok = kr != NULL && kr->status == XCB_GRAB_STATUS_SUCCESS;
	free(kr);
	if (!kb_ok) {
		xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
		return false;
	}

	return true;
}

static void ungrab_input(void)
{
	xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
	xcb_ungrab_keyboard(wm.conn, XCB_CURRENT_TIME);
}

/* ================================================================== *
 * 描画 (SPEC §4.1, §4.2, §4.3)
 * ================================================================== */

static void draw_separator(int i)
{
	int x = content_x() + 3;
	int w = content_w() - 6;
	int y = g_menu.item_top[i] + (g_menu.item_h[i] - 2) / 2;

	if (w < 0)
		w = 0;
	draw_bevel(g_menu.win, (int16_t)x, (int16_t)y, (uint16_t)w, 2, BEVEL_BUMP);
}

static void draw_one_item(int i, bool selected)
{
	const struct menu_item *it = &g_menu.items[i];
	int x = content_x();
	int w = content_w();
	int y = g_menu.item_top[i];
	int h = g_menu.item_h[i];
	int fh, base_y, text_x;
	uint32_t item_bg, text_color;
	size_t label_len;

	if (it->separator) {
		draw_rect(g_menu.win, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h,
		          wm.cfg.color[THEME_MENU_BG]);
		draw_separator(i);
		return;
	}

	item_bg = (selected && it->enabled) ? wm.cfg.color[THEME_HIGHLIGHT]
	                                    : wm.cfg.color[THEME_MENU_BG];
	draw_rect(g_menu.win, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, item_bg);

	fh = (int)font_height();
	base_y = y + (h - fh) / 2 + (int)font_ascent();
	text_x = x + TEXT_LEFT_PAD;
	label_len = strlen(it->label);

	if (!it->enabled) {
		/* SPEC §4.1: 無効項目は disabled_text + 1px hilight オフセットのエンボス */
		font_draw(g_menu.win, (int16_t)(text_x + 1), (int16_t)(base_y + 1),
		         it->label, label_len, wm.cfg.color[THEME_HILIGHT]);
		font_draw(g_menu.win, (int16_t)text_x, (int16_t)base_y,
		         it->label, label_len, wm.cfg.color[THEME_DISABLED_TEXT]);
	} else {
		text_color = selected ? wm.cfg.color[THEME_HIGHLIGHT_TEXT]
		                      : wm.cfg.color[THEME_MENU_TEXT];
		font_draw(g_menu.win, (int16_t)text_x, (int16_t)base_y,
		         it->label, label_len, text_color);
	}

	if (it->accel) {
		size_t accel_len = strlen(it->accel);
		int accel_x = x + w - TEXT_RIGHT_PAD - (int)g_menu.accel_w[i];

		if (!it->enabled) {
			font_draw(g_menu.win, (int16_t)(accel_x + 1), (int16_t)(base_y + 1),
			         it->accel, accel_len, wm.cfg.color[THEME_HILIGHT]);
			font_draw(g_menu.win, (int16_t)accel_x, (int16_t)base_y,
			         it->accel, accel_len, wm.cfg.color[THEME_DISABLED_TEXT]);
		} else {
			uint32_t ac = selected ? wm.cfg.color[THEME_HIGHLIGHT_TEXT]
			                       : wm.cfg.color[THEME_MENU_TEXT];
			font_draw(g_menu.win, (int16_t)accel_x, (int16_t)base_y,
			         it->accel, accel_len, ac);
		}
	}
}

/*
 * 縦バナー (SPEC §4.9)。
 *
 * 上が shadow (#808080)、下が dkshadow (#000000) の縦グラデーションに、
 * 名称を 1 文字ずつ縦に積んで描く。コアプロトコルの PolyText16 には
 * 文字の回転が無いため、Win98 の 90 度回転そのものは再現できない
 * （回転させるにはグリフを 1 枚ずつ Pixmap へ描いて転置する必要があり、
 *  往復とメモリの両面で §9 の予算に見合わない）。ここは意図的な差異。
 *
 * グラデーションは §4.4 と同じ「色が実際に変わる境界でだけ分割する」方式。
 * shadow→dkshadow は 129 段で、メニューを開いた時にしか描かないので
 * リクエスト量は問題にならない。
 */
static void draw_banner(void)
{
	uint32_t top = wm.cfg.color[THEME_SHADOW];
	uint32_t bot = wm.cfg.color[THEME_DKSHADOW];
	int h = (int)g_menu.h - 2 * MENU_BORDER;
	int y0 = MENU_BORDER;
	int x0 = MENU_BORDER;
	int w = (int)g_menu.banner_w;
	int i, n;
	const char *name = WM_DISPLAY_NAME;

	if (w <= 0 || h <= 0)
		return;

	/* 段数 = 3 チャネルの変化量の最大 + 1。各段は等幅で割り付ける */
	{
		int dr = abs((int)((top >> 16) & 0xff) - (int)((bot >> 16) & 0xff));
		int dg = abs((int)((top >> 8) & 0xff) - (int)((bot >> 8) & 0xff));
		int db = abs((int)(top & 0xff) - (int)(bot & 0xff));
		n = dr > dg ? dr : dg;
		if (db > n)
			n = db;
		n += 1;
		if (n > h)
			n = h;
		if (n < 1)
			n = 1;
	}

	for (i = 0; i < n; i++) {
		int band_y = y0 + (int)((int64_t)h * i / n);
		int band_h = y0 + (int)((int64_t)h * (i + 1) / n) - band_y;
		uint32_t col = 0;
		int ch;

		for (ch = 0; ch < 3; ch++) {
			int shift = 16 - ch * 8;
			int a = (int)((top >> shift) & 0xff);
			int b = (int)((bot >> shift) & 0xff);
			int v = n > 1 ? a + (b - a) * i / (n - 1) : a;
			col |= (uint32_t)v << shift;
		}
		if (band_h > 0)
			draw_rect(g_menu.win, (int16_t)x0, (int16_t)band_y,
			          (uint16_t)w, (uint16_t)band_h, col);
	}

	/* 名称を 1 文字ずつ縦に積む。下端から上へ向かって並べる */
	{
		int fh = (int)font_height();
		int len = (int)strlen(name);
		int total, cy;

		if (fh <= 0)
			return;
		total = len * fh;
		if (total > h - 8)
			len = (h - 8) / fh;      /* 入らない分は捨てる */
		if (len <= 0)
			return;

		cy = y0 + h - 6 - len * fh;
		for (i = 0; i < len; i++) {
			char cbuf[2];
			uint16_t cw;

			cbuf[0] = name[i];
			cbuf[1] = '\0';
			cw = font_text_width(cbuf, 1);
			font_draw(g_menu.win,
			          (int16_t)(x0 + (w - (int)cw) / 2),
			          (int16_t)(cy + i * fh + (int)font_ascent()),
			          cbuf, 1, wm.cfg.color[THEME_HILIGHT]);
		}
	}
}

static void draw_menu(void)
{
	int i;

	if (g_menu.win == XCB_NONE)
		return;

	draw_rect(g_menu.win, 0, 0, g_menu.w, g_menu.h, wm.cfg.color[THEME_MENU_BG]);
	draw_bevel(g_menu.win, 0, 0, g_menu.w, g_menu.h, BEVEL_RAISED);

	if (g_menu.banner_w > 0)
		draw_banner();

	for (i = 0; i < g_menu.n_items; i++)
		draw_one_item(i, i == g_menu.selected);
}

static void select_index(int idx)
{
	if (idx == g_menu.selected)
		return;
	g_menu.selected = idx;
	draw_menu();
}

/* ================================================================== *
 * 入力処理
 * ================================================================== */

static int item_at_y(int local_y)
{
	int i;

	for (i = 0; i < g_menu.n_items; i++) {
		if (local_y >= g_menu.item_top[i] &&
		    local_y < g_menu.item_top[i] + g_menu.item_h[i])
			return i;
	}
	return -1;
}

/* menu_close() で g_menu がクリアされる前に action と対象 client を退避する */
static void activate_index(int i)
{
	uint8_t action;
	const char *arg;
	struct client *c;
	static char argbuf[512];

	if (!item_selectable(i))
		return;

	action = g_menu.items[i].action;
	arg = g_menu.items[i].arg;
	c = g_menu.client;

	/*
	 * arg は cfg->start[] が所有する文字列を借りている。menu_close() は
	 * cfg を触らないので現状は生き残るが、SIGHUP による設定再読み込みと
	 * 競合しうるため、閉じる前に控えを取ってから使う。
	 */
	if (arg != NULL) {
		snprintf(argbuf, sizeof argbuf, "%s", arg);
		arg = argbuf;
	}

	/*
	 * 先に閉じて grab を手放す。ACT_MOVE_KB / ACT_RESIZE_KB は move.c が
	 * 自前でポインタを grab するため、メニューの grab が残ったままだと
	 * 二重 grab で失敗する。
	 */
	menu_close();
	input_run_action(action, arg, c);
}

static void handle_motion(int16_t root_x, int16_t root_y)
{
	int lx = root_x - g_menu.x;
	int ly = root_y - g_menu.y;
	int idx;

	if (lx < 0 || ly < 0 || lx >= (int)g_menu.w || ly >= (int)g_menu.h) {
		select_index(-1);
		return;
	}
	idx = item_at_y(ly);
	select_index(item_selectable(idx) ? idx : -1);
}

static void handle_button(int16_t root_x, int16_t root_y)
{
	int lx = root_x - g_menu.x;
	int ly = root_y - g_menu.y;
	int idx;

	if (lx < 0 || ly < 0 || lx >= (int)g_menu.w || ly >= (int)g_menu.h) {
		menu_close();          /* 外側クリック = 閉じる (SPEC §4.6) */
		return;
	}
	idx = item_at_y(ly);
	if (item_selectable(idx))
		activate_index(idx);
}

static void handle_key(xcb_key_press_event_t *e)
{
	xcb_keysym_t sym;
	int i;

	if (!g_menu.keysyms)
		return;
	sym = xcb_key_symbols_get_keysym(g_menu.keysyms, e->detail, 0);
	if (sym == XCB_NO_SYMBOL)
		return;

	switch (sym) {
	case XK_Escape:
		menu_close();
		return;
	case XK_Down:
		select_index(g_menu.selected < 0 ? first_selectable()
		                                 : next_selectable(g_menu.selected, 1));
		return;
	case XK_Up:
		select_index(g_menu.selected < 0 ? last_selectable()
		                                 : next_selectable(g_menu.selected, -1));
		return;
	case XK_Return:
	case XK_KP_Enter:
		activate_index(g_menu.selected);
		return;
	default:
		break;
	}

	/* アクセラレータ文字によるジャンプ。ラテン英数字の範囲のみ (SPEC §4.6) */
	if (sym >= 0x20 && sym <= 0x7e) {
		char ch = (char)toupper((int)sym);

		for (i = 0; i < g_menu.n_items; i++) {
			if (item_selectable(i) && item_accel_letter(&g_menu.items[i]) == ch) {
				activate_index(i);
				return;
			}
		}
	}
}

/* ================================================================== *
 * 公開 API (汎用ポップアップ)
 * ================================================================== */

bool menu_active(void)
{
	return g_menu.active;
}

void menu_close(void)
{
	if (!g_menu.active)
		return;

	ungrab_input();

	if (g_menu.keysyms) {
		xcb_key_symbols_free(g_menu.keysyms);
		g_menu.keysyms = NULL;
	}
	if (g_menu.win != XCB_NONE) {
		xcb_destroy_window(wm.conn, g_menu.win);
		g_menu.win = XCB_NONE;
	}

	g_menu.active = false;
	g_menu.client = NULL;
	g_menu.n_items = 0;
	g_menu.selected = -1;

	if (g_menu.is_start) {
		g_menu.is_start = false;
		g_menu.banner_w = 0;
		taskbar_start_closed();   /* スタートボタンの押し込みを戻す */
	}

	xcb_flush(wm.conn);
}

bool menu_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & 0x7f;

	switch (type) {
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *)ev;

		if (e->window != g_menu.win)
			return false;
		if (e->count != 0)
			return true;    /* 連続 Expose の最後だけ描く */
		draw_menu();
		return true;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;

		handle_motion(e->root_x, e->root_y);
		return true;
	}
	case XCB_BUTTON_PRESS: {
		xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;

		handle_button(e->root_x, e->root_y);
		return true;
	}
	case XCB_BUTTON_RELEASE:
		return true;            /* 活性化は Press 側で済んでいる。飲み込むだけ */
	case XCB_KEY_PRESS:
		handle_key((xcb_key_press_event_t *)ev);
		return true;
	case XCB_KEY_RELEASE:
		return true;
	default:
		return false;
	}
}

/* ================================================================== *
 * ウィンドウメニューの内容 (SPEC §4.6)
 * ================================================================== */

static void set_item(int idx, const char *label, uint8_t action,
                     const char *accel, bool enabled)
{
	struct menu_item *it = &g_menu.items[idx];

	it->label = label;
	it->action = action;
	it->arg = NULL;
	it->accel = accel;
	it->enabled = enabled;
	it->separator = false;
}

/* ACT_EXEC の項目。arg は cfg->start[] の文字列を借りるだけで所有しない */
static void set_exec_item(int idx, const char *label, const char *cmd)
{
	struct menu_item *it = &g_menu.items[idx];

	it->label = label;
	it->action = ACT_EXEC;
	it->arg = cmd;
	it->accel = NULL;
	it->enabled = true;
	it->separator = false;
}

static void set_separator(int idx)
{
	struct menu_item *it = &g_menu.items[idx];

	it->label = NULL;
	it->action = ACT_NONE;
	it->arg = NULL;
	it->accel = NULL;
	it->enabled = false;
	it->separator = true;
}

/*
 * 項目・順序・区切り位置は SPEC §4.6 のとおり。
 *
 * 「元のサイズに戻す」「最大化」は ACT_MAXIMIZE 1 つを共有する
 * (input_run_action の ACT_MAXIMIZE は現在の states を見てトグルする実装
 * になっており、意味的にはそれぞれ「オフにする」「オンにする」に一致する。
 * ACT_* に個別の「復元」動作は無い — w98wm.h のギャップ、詳細はレポート参照)。
 * 「移動」「サイズ変更」はキーボード操作版 (ACT_MOVE_KB / ACT_RESIZE_KB) を使う。
 */
static void build_window_menu_items(struct client *c)
{
	const struct type_props *tp = type_props(c->type);
	bool maximized  = (c->states & ST_MAXIMIZED) != 0;
	bool fixed      = (c->flags & CF_FIXED_SIZE) != 0;
	bool fullscreen = (c->states & ST_FULLSCREEN) != 0;
	int n = 0;

	set_item(n++, "元のサイズに戻す(R)", ACT_MAXIMIZE, NULL,
	        maximized && !fullscreen);
	set_item(n++, "移動(M)", ACT_MOVE_KB, NULL,
	        !maximized && !fullscreen);
	set_item(n++, "サイズ変更(S)", ACT_RESIZE_KB, NULL,
	        !fixed && !maximized && !fullscreen);
	set_item(n++, "最小化(N)", ACT_MINIMIZE, NULL,
	        tp->in_taskbar);
	set_item(n++, "最大化(X)", ACT_MAXIMIZE, NULL,
	        !fixed && !maximized && !fullscreen);
	set_separator(n++);
	set_item(n++, "閉じる(C)", ACT_CLOSE, "Alt+F4", true);

	g_menu.n_items = n;
}

void menu_open_window_menu(struct client *c, int16_t root_x, int16_t root_y)
{
	if (!c)
		return;
	if (g_menu.active)
		menu_close();

	build_window_menu_items(c);
	if (g_menu.n_items <= 0)
		return;

	compute_layout();
	position_popup(root_x, root_y);
	create_popup_window();

	if (!grab_input()) {
		xcb_destroy_window(wm.conn, g_menu.win);
		g_menu.win = XCB_NONE;
		xcb_flush(wm.conn);
		return;
	}

	g_menu.client = c;
	g_menu.selected = -1;
	g_menu.active = true;
	g_menu.keysyms = xcb_key_symbols_alloc(wm.conn);

	xcb_flush(wm.conn);
}

/* ================================================================== *
 * スタートメニュー (SPEC §4.9)
 *
 * 項目は `~/.config/<WM_CONFIG_DIR>/menu` の内容（config.c が読む）に、
 * WM 自身の操作を組み込み項目として足したもの。XDG の .desktop 走査は
 * §4.9 の決定どおり行わない。
 *
 * v1.0 の制限: **階層メニューは未実装**（フラットな 1 段のみ）。
 * ポップアップを入れ子にすると grab の受け渡しと親メニューの選択保持が
 * 要るため、Phase 4 の範囲からは外した。SPEC §4.9 との差異として記録する。
 * ================================================================== */

static void build_start_menu_items(void)
{
	int n = 0;
	uint16_t i;

	for (i = 0; i < wm.cfg.n_start && n < MENU_MAX_ITEMS - 4; i++) {
		if (wm.cfg.start[i].label == NULL) {
			if (n > 0 && !g_menu.items[n - 1].separator)
				set_separator(n++);
		} else {
			set_exec_item(n, wm.cfg.start[i].label, wm.cfg.start[i].cmd);
			n++;
		}
	}

	if (n > 0 && !g_menu.items[n - 1].separator)
		set_separator(n++);

	set_item(n++, "デスクトップの表示(D)", ACT_SHOW_DESKTOP, NULL, true);
	set_separator(n++);
	set_item(n++, "終了(U)", ACT_QUIT, NULL, true);

	g_menu.n_items = n;
}

void menu_open_start(int16_t x, int16_t bottom_y)
{
	if (g_menu.active)
		menu_close();

	g_menu.is_start = true;
	g_menu.banner_w = (uint16_t)(BANNER_W * theme_metrics()->scale);

	build_start_menu_items();
	if (g_menu.n_items <= 0) {
		g_menu.is_start = false;
		g_menu.banner_w = 0;
		taskbar_start_closed();
		return;
	}

	compute_layout();

	/*
	 * ウィンドウメニューと違い、基準は**左下**。タスクバーの上へ生やす。
	 * position_popup() は左上を基準に作業領域へ収めるので、
	 * 高さの分だけ持ち上げてから渡す。
	 */
	position_popup(x, (int16_t)(bottom_y - (int16_t)g_menu.h));
	create_popup_window();

	if (!grab_input()) {
		xcb_destroy_window(wm.conn, g_menu.win);
		g_menu.win = XCB_NONE;
		g_menu.is_start = false;
		g_menu.banner_w = 0;
		g_menu.n_items = 0;
		taskbar_start_closed();
		xcb_flush(wm.conn);
		return;
	}

	g_menu.client = NULL;
	g_menu.selected = -1;
	g_menu.active = true;
	g_menu.keysyms = xcb_key_symbols_alloc(wm.conn);

	xcb_flush(wm.conn);
}
