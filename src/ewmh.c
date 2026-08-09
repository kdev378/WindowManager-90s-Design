/*
 * ewmh.c - EWMH プロパティの書き出しとクライアントメッセージの処理
 *
 * 対応する仕様: docs/SPEC.md §5.2, §5.2.1, §5.2.2, §3.5.2, §3.7.1, §3.8, §7.2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

/* ================================================================== *
 * プロパティ書き込みの小さなラッパ
 * ================================================================== */

static void set_card32(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type,
                        uint32_t value)
{
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, w, prop, type,
	                     32, 1, &value);
}

static void set_card32_array(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type,
                              const uint32_t *values, uint32_t n)
{
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, w, prop, type,
	                     32, n, values);
}

static void set_utf8(xcb_window_t w, xcb_atom_t prop, const char *s, size_t len)
{
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, w, prop,
	                     atoms[ATOM_UTF8_STRING], 8, (uint32_t)len, s);
}

/* ================================================================== *
 * ewmh_init / ewmh_update_supported
 * ================================================================== */

void ewmh_init(void)
{
	wm.check_win = xcb_generate_id(wm.conn);

	uint32_t mask = XCB_CW_OVERRIDE_REDIRECT;
	uint32_t values[1] = { 1 };

	/* 1x1 の InputOutput ウィンドウ。画面外に置き、override_redirect で
	 * WM 自身のイベント処理・スタッキング対象から外す (EWMH _NET_SUPPORTING_WM_CHECK)。 */
	xcb_create_window(wm.conn, wm.depth, wm.check_win, wm.root,
	                   -1, -1, 1, 1, 0,
	                   XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                   mask, values);

	/* ルートと自分自身の両方に _NET_SUPPORTING_WM_CHECK = check_win を置く。
	 * pager 等はこれで「動いている WM が存在するか」を確認する。 */
	set_card32(wm.root, atoms[ATOM_NET_SUPPORTING_WM_CHECK],
	           XCB_ATOM_WINDOW, wm.check_win);
	set_card32(wm.check_win, atoms[ATOM_NET_SUPPORTING_WM_CHECK],
	           XCB_ATOM_WINDOW, wm.check_win);

	/* _NET_WM_NAME (UTF8_STRING) */
	set_utf8(wm.check_win, atoms[ATOM_NET_WM_NAME],
	         WM_DISPLAY_NAME, strlen(WM_DISPLAY_NAME));

	/* WM_CLASS: "instance\0class\0" の STRING */
	char class_buf[sizeof(WM_CLASS_INSTANCE) + sizeof(WM_CLASS_CLASS)];
	size_t inst_len = sizeof(WM_CLASS_INSTANCE); /* NUL 込み */
	size_t class_len = sizeof(WM_CLASS_CLASS);   /* NUL 込み */
	memcpy(class_buf, WM_CLASS_INSTANCE, inst_len);
	memcpy(class_buf + inst_len, WM_CLASS_CLASS, class_len);
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.check_win,
	                     XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
	                     (uint32_t)(inst_len + class_len), class_buf);

	ewmh_update_supported();
	/* 起動時から存在させる。pager は値の有無で対応を判断する */
	ewmh_update_showing_desktop();
}

void ewmh_update_supported(void)
{
	xcb_atom_t list[ATOM_COUNT];
	uint32_t n = atoms_supported_list(list, ATOM_COUNT);

	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
	                     atoms[ATOM_NET_SUPPORTED], XCB_ATOM_ATOM, 32,
	                     n, list);
}

/* ================================================================== *
 * ewmh_update_desktop_props (SPEC §3.8)
 * ================================================================== */

void ewmh_update_desktop_props(void)
{
	set_card32(wm.root, atoms[ATOM_NET_NUMBER_OF_DESKTOPS],
	           XCB_ATOM_CARDINAL, wm.n_desktops);
	set_card32(wm.root, atoms[ATOM_NET_CURRENT_DESKTOP],
	           XCB_ATOM_CARDINAL, wm.current_desktop);

	/* 仮想デスクトップはパン（viewport スクロール）しないため、
	 * geometry はルートスクリーンのサイズ、viewport は全面 (0,0) 固定 (§3.8) */
	uint32_t geom[2] = { wm.screen->width_in_pixels,
	                      wm.screen->height_in_pixels };
	set_card32_array(wm.root, atoms[ATOM_NET_DESKTOP_GEOMETRY],
	                  XCB_ATOM_CARDINAL, geom, 2);

	uint32_t n_desk = wm.n_desktops;
	if (n_desk == 0)
		n_desk = 1;
	if (n_desk > WM_MAX_DESKTOPS)
		n_desk = WM_MAX_DESKTOPS;

	uint32_t viewport[WM_MAX_DESKTOPS * 2];
	for (uint32_t i = 0; i < n_desk; i++) {
		viewport[i * 2]     = 0;
		viewport[i * 2 + 1] = 0;
	}
	set_card32_array(wm.root, atoms[ATOM_NET_DESKTOP_VIEWPORT],
	                  XCB_ATOM_CARDINAL, viewport, n_desk * 2);

	/* _NET_DESKTOP_NAMES: 名前を保持するフィールドが struct wm に無いため
	 * "Desktop N" (1-origin) を機械的に生成する。UTF8_STRING の NUL 区切り列。 */
	char namebuf[WM_MAX_DESKTOPS * 32];
	size_t off = 0;
	for (uint32_t i = 0; i < n_desk && off < sizeof(namebuf); i++) {
		int written = snprintf(namebuf + off, sizeof(namebuf) - off,
		                        "Desktop %u", i + 1);
		if (written < 0)
			break;
		off += (size_t)written;
		if (off < sizeof(namebuf)) {
			namebuf[off] = '\0';
			off += 1;
		}
	}
	set_utf8(wm.root, atoms[ATOM_NET_DESKTOP_NAMES], namebuf, off);
}

/* ================================================================== *
 * ewmh_update_workarea (SPEC §3.5.2)
 *
 * ルートの _NET_WORKAREA は「全モニタの外接矩形から、外周 4 辺に接する
 * strut を引いた 1 矩形」。strut の生データは layout.c 内部にしか無いため
 * (struct client に strut フィールドが無い)、layout.c が既に計算済みの
 * per-monitor workarea との差分 (= そのモニタの当該辺に適用された strut 幅)
 * を、外接矩形の外周と一致するモニタについてのみ読み取って合成する。
 * これにより「per-monitor workarea を再計算しない」制約を満たしつつ
 * §3.5.2 のルート用ルールを導出できる。
 * ================================================================== */

void ewmh_update_workarea(void)
{
	struct rect rect = { 0, 0, 0, 0 };

	if (wm.n_monitors > 0) {
		int32_t minx = INT32_MAX, miny = INT32_MAX;
		int32_t maxx = INT32_MIN, maxy = INT32_MIN;

		for (uint8_t i = 0; i < wm.n_monitors; i++) {
			const struct monitor *m = &wm.monitors[i];
			int32_t x0 = m->geom.x, y0 = m->geom.y;
			int32_t x1 = x0 + m->geom.w, y1 = y0 + m->geom.h;
			if (x0 < minx) minx = x0;
			if (y0 < miny) miny = y0;
			if (x1 > maxx) maxx = x1;
			if (y1 > maxy) maxy = y1;
		}

		int32_t inset_l = 0, inset_r = 0, inset_t = 0, inset_b = 0;
		for (uint8_t i = 0; i < wm.n_monitors; i++) {
			const struct monitor *m = &wm.monitors[i];
			int32_t gx0 = m->geom.x, gy0 = m->geom.y;
			int32_t gx1 = gx0 + m->geom.w, gy1 = gy0 + m->geom.h;
			int32_t wx0 = m->workarea.x, wy0 = m->workarea.y;
			int32_t wx1 = wx0 + m->workarea.w, wy1 = wy0 + m->workarea.h;

			if (gx0 == minx) {
				int32_t d = wx0 - gx0;
				if (d > inset_l) inset_l = d;
			}
			if (gx1 == maxx) {
				int32_t d = gx1 - wx1;
				if (d > inset_r) inset_r = d;
			}
			if (gy0 == miny) {
				int32_t d = wy0 - gy0;
				if (d > inset_t) inset_t = d;
			}
			if (gy1 == maxy) {
				int32_t d = gy1 - wy1;
				if (d > inset_b) inset_b = d;
			}
		}

		int32_t x = minx + inset_l;
		int32_t y = miny + inset_t;
		int32_t w = (maxx - minx) - inset_l - inset_r;
		int32_t h = (maxy - miny) - inset_t - inset_b;
		if (w < 0) w = 0;
		if (h < 0) h = 0;

		rect.x = (int16_t)x;
		rect.y = (int16_t)y;
		rect.w = (uint16_t)w;
		rect.h = (uint16_t)h;
	}

	uint32_t n_desk = wm.n_desktops;
	if (n_desk == 0)
		n_desk = 1;
	if (n_desk > WM_MAX_DESKTOPS)
		n_desk = WM_MAX_DESKTOPS;

	uint32_t work[WM_MAX_DESKTOPS * 4];
	for (uint32_t i = 0; i < n_desk; i++) {
		work[i * 4]     = (uint32_t)(int32_t)rect.x;
		work[i * 4 + 1] = (uint32_t)(int32_t)rect.y;
		work[i * 4 + 2] = rect.w;
		work[i * 4 + 3] = rect.h;
	}
	set_card32_array(wm.root, atoms[ATOM_NET_WORKAREA], XCB_ATOM_CARDINAL,
	                  work, n_desk * 4);
}

/* ================================================================== *
 * ewmh_update_active_window (SPEC §3.6)
 * ================================================================== */

void ewmh_update_active_window(void)
{
	xcb_window_t w = wm.focused != NULL ? wm.focused->win : XCB_WINDOW_NONE;
	set_card32(wm.root, atoms[ATOM_NET_ACTIVE_WINDOW], XCB_ATOM_WINDOW, w);
}

/* ================================================================== *
 * ewmh_set_wm_state (SPEC §5.2)
 * ================================================================== */

void ewmh_set_wm_state(struct client *c)
{
	xcb_atom_t list[13];
	uint32_t n = 0;

	if (c->states & ST_MODAL)             list[n++] = atoms[ATOM_NET_WM_STATE_MODAL];
	if (c->states & ST_STICKY)            list[n++] = atoms[ATOM_NET_WM_STATE_STICKY];
	if (c->states & ST_MAXIMIZED_VERT)    list[n++] = atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT];
	if (c->states & ST_MAXIMIZED_HORZ)    list[n++] = atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ];
	if (c->states & ST_SHADED)            list[n++] = atoms[ATOM_NET_WM_STATE_SHADED];
	if (c->states & ST_SKIP_TASKBAR)      list[n++] = atoms[ATOM_NET_WM_STATE_SKIP_TASKBAR];
	if (c->states & ST_SKIP_PAGER)        list[n++] = atoms[ATOM_NET_WM_STATE_SKIP_PAGER];
	if (c->states & ST_HIDDEN)            list[n++] = atoms[ATOM_NET_WM_STATE_HIDDEN];
	if (c->states & ST_FULLSCREEN)        list[n++] = atoms[ATOM_NET_WM_STATE_FULLSCREEN];
	if (c->states & ST_ABOVE)             list[n++] = atoms[ATOM_NET_WM_STATE_ABOVE];
	if (c->states & ST_BELOW)             list[n++] = atoms[ATOM_NET_WM_STATE_BELOW];
	if (c->states & ST_DEMANDS_ATTENTION) list[n++] = atoms[ATOM_NET_WM_STATE_DEMANDS_ATTENTION];
	if (c->states & ST_FOCUSED)           list[n++] = atoms[ATOM_NET_WM_STATE_FOCUSED];

	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->win,
	                     atoms[ATOM_NET_WM_STATE], XCB_ATOM_ATOM, 32,
	                     n, list);
}

/* ================================================================== *
 * ewmh_set_allowed_actions
 * ================================================================== */

void ewmh_set_allowed_actions(struct client *c)
{
	xcb_atom_t list[6];
	uint32_t n = 0;

	list[n++] = atoms[ATOM_NET_WM_ACTION_MOVE];   /* 常に可 */
	list[n++] = atoms[ATOM_NET_WM_ACTION_CLOSE];  /* 常に可 */

	if (!(c->flags & CF_FIXED_SIZE)) {
		list[n++] = atoms[ATOM_NET_WM_ACTION_RESIZE];
		list[n++] = atoms[ATOM_NET_WM_ACTION_MAXIMIZE_HORZ];
		list[n++] = atoms[ATOM_NET_WM_ACTION_MAXIMIZE_VERT];
	}

	/* タスクバーに出ない種別（DOCK/TOOLTIP 等）は最小化の意味を持たない */
	if (type_props(c->type)->in_taskbar)
		list[n++] = atoms[ATOM_NET_WM_ACTION_MINIMIZE];

	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->win,
	                     atoms[ATOM_NET_WM_ALLOWED_ACTIONS],
	                     XCB_ATOM_ATOM, 32, n, list);
}

/* ================================================================== *
 * ewmh_set_frame_extents (SPEC §7.2)
 * ================================================================== */

void ewmh_set_frame_extents(struct client *c)
{
	uint16_t left = 0, right = 0, top = 0, bottom = 0;

	/* CSD クライアントは WM 側の装飾を持たないため常に 0 (§7.2) */
	if (!(c->flags & CF_CSD))
		client_frame_offsets(c, &left, &right, &top, &bottom);

	uint32_t vals[4] = { left, right, top, bottom };
	set_card32_array(c->win, atoms[ATOM_NET_FRAME_EXTENTS],
	                  XCB_ATOM_CARDINAL, vals, 4);
}

/* ================================================================== *
 * ewmh_handle_client_message (SPEC §5.2, §3.8, ICCCM §4.1.4)
 * ================================================================== */

/* _NET_WM_STATE のアクション値 (EWMH) */
enum { NET_WM_STATE_REMOVE = 0, NET_WM_STATE_ADD = 1, NET_WM_STATE_TOGGLE = 2 };

static bool resolve_on(uint32_t action, bool currently_set)
{
	switch (action) {
	case NET_WM_STATE_REMOVE: return false;
	case NET_WM_STATE_ADD:    return true;
	case NET_WM_STATE_TOGGLE: return !currently_set;
	default:                  return currently_set;
	}
}

static void apply_bit(struct client *c, uint32_t action, uint32_t bit)
{
	switch (action) {
	case NET_WM_STATE_REMOVE: c->states &= ~bit; break;
	case NET_WM_STATE_ADD:    c->states |= bit;  break;
	case NET_WM_STATE_TOGGLE: c->states ^= bit;  break;
	default: break;
	}
}

/*
 * 専用ヘルパを持たない状態 (MODAL, STICKY, SKIP_TASKBAR, SKIP_PAGER,
 * ABOVE, BELOW, DEMANDS_ATTENTION) をビットだけ更新する。
 * SHADED / HIDDEN は専用の遷移ロジックが無いため（HIDDEN は WM 内部専用、
 * §3.8）ここでは意図的に対象外とし、クライアントからの直接要求は無視する。
 * 戻り値は「ビットを変更したか」。
 */
static bool apply_simple_state(struct client *c, uint32_t action, int idx)
{
	uint32_t bit;

	switch (idx) {
	case ATOM_NET_WM_STATE_MODAL:             bit = ST_MODAL; break;
	case ATOM_NET_WM_STATE_STICKY:            bit = ST_STICKY; break;
	case ATOM_NET_WM_STATE_SKIP_TASKBAR:      bit = ST_SKIP_TASKBAR; break;
	case ATOM_NET_WM_STATE_SKIP_PAGER:        bit = ST_SKIP_PAGER; break;
	case ATOM_NET_WM_STATE_ABOVE:             bit = ST_ABOVE; break;
	case ATOM_NET_WM_STATE_BELOW:             bit = ST_BELOW; break;
	case ATOM_NET_WM_STATE_DEMANDS_ATTENTION: bit = ST_DEMANDS_ATTENTION; break;
	default: return false;
	}

	apply_bit(c, action, bit);
	return true;
}

static void handle_net_wm_state(xcb_client_message_event_t *ev, struct client *c)
{
	uint32_t action = ev->data.data32[0];
	xcb_atom_t a1 = (xcb_atom_t)ev->data.data32[1];
	xcb_atom_t a2 = (xcb_atom_t)ev->data.data32[2];
	xcb_atom_t atoms_req[2] = { a1, a2 };

	bool want_vert = false, want_horz = false;
	bool simple_changed = false;

	for (int i = 0; i < 2; i++) {
		xcb_atom_t a = atoms_req[i];
		if (a == XCB_ATOM_NONE)
			continue;

		int idx = atoms_lookup(a);
		if (idx < 0)
			continue;

		if (idx == ATOM_NET_WM_STATE_MAXIMIZED_VERT) {
			want_vert = true;
			continue;
		}
		if (idx == ATOM_NET_WM_STATE_MAXIMIZED_HORZ) {
			want_horz = true;
			continue;
		}
		if (idx == ATOM_NET_WM_STATE_FULLSCREEN) {
			bool on = resolve_on(action, (c->states & ST_FULLSCREEN) != 0);
			layout_fullscreen(c, on);
			continue;
		}

		if (apply_simple_state(c, action, idx))
			simple_changed = true;
	}

	if (want_vert || want_horz) {
		bool currently;
		if (want_vert && want_horz)
			currently = (c->states & ST_MAXIMIZED_VERT) &&
			            (c->states & ST_MAXIMIZED_HORZ);
		else if (want_vert)
			currently = (c->states & ST_MAXIMIZED_VERT) != 0;
		else
			currently = (c->states & ST_MAXIMIZED_HORZ) != 0;

		bool on = resolve_on(action, currently);
		layout_maximize(c, want_vert, want_horz, on);
	}

	if (simple_changed) {
		stack_apply();
		ewmh_set_wm_state(c);
	}
}


/* ================================================================== *
 * デスクトップの表示 (_NET_SHOWING_DESKTOP)
 *
 * 自分が隠したウィンドウだけを記録しておく。これが無いと、
 * 元から最小化されていたウィンドウまで復元してしまう。
 * ================================================================== */
static bool showing_desktop;
static xcb_window_t hidden_by_us[WM_MAX_CLIENTS];
static uint16_t n_hidden;

void ewmh_update_showing_desktop(void)
{
	uint32_t v = showing_desktop ? 1u : 0u;
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
		atoms[ATOM_NET_SHOWING_DESKTOP], XCB_ATOM_CARDINAL, 32, 1, &v);
}

static void showing_desktop_set(bool on)
{
	struct client *c;
	uint16_t i;

	if (on == showing_desktop)
		return;
	showing_desktop = on;

	if (on) {
		n_hidden = 0;
		for (c = wm.stack_bottom; c; c = c->next) {
			if (c->flags & CF_ICONIC)
				continue;                  /* 元から最小化。触らない */
			if (c->type == TYPE_DESKTOP || c->type == TYPE_DOCK)
				continue;
			if (!client_visible_on(c, wm.current_desktop))
				continue;
			if (n_hidden < WM_MAX_CLIENTS)
				hidden_by_us[n_hidden++] = c->win;
			client_iconify(c);
		}
	} else {
		for (i = 0; i < n_hidden; i++) {
			c = client_find(hidden_by_us[i]);
			if (c != NULL)
				client_deiconify(c);
		}
		n_hidden = 0;
	}
	ewmh_update_showing_desktop();
	stack_apply();
}

/* キーバインド (ACT_SHOW_DESKTOP) とスタートメニューからのトグル (§4.8) */
void ewmh_toggle_showing_desktop(void)
{
	showing_desktop_set(!showing_desktop);
}

/* ================================================================== *
 * フォーカススティール防止 (SPEC §3.6)
 * ================================================================== */

void ewmh_set_demands_attention(struct client *c, bool on)
{
	if (on)
		c->states |= ST_DEMANDS_ATTENTION;
	else
		c->states &= ~(uint32_t)ST_DEMANDS_ATTENTION;
	ewmh_set_wm_state(c);
	taskbar_update();   /* タスクボタンの点滅 (§4.8) */
}

bool ewmh_allow_activation(struct client *c, uint32_t source, xcb_timestamp_t t)
{
	uint32_t utime = c->user_time;

	/* source 2 = pager。利用者の明示操作なので常に許可 (EWMH) */
	if (source == 2)
		return true;

	/*
	 * _NET_WM_USER_TIME_WINDOW が指定されていれば、user time は
	 * そちらのウィンドウのプロパティに載っている (§3.6)。
	 */
	if (c->user_time_win != XCB_WINDOW_NONE) {
		uint32_t v;
		if (prop_get_card32(c->user_time_win, atoms[ATOM_NET_WM_USER_TIME],
		                    XCB_ATOM_CARDINAL, &v))
			utime = v;
	}

	/* user_time == 0 は「このウィンドウはフォーカスを望まない」の明示 */
	if (utime == 0 && c->user_time_win != XCB_WINDOW_NONE) {
		ewmh_set_demands_attention(c, true);
		return false;
	}

	/*
	 * 直近の利用者操作より古い要求は自己アクティブ化とみなして拒否する。
	 * X のタイムスタンプは 32bit で巻き戻るため、差の符号で比較する。
	 */
	if (utime != 0 && wm.last_time != 0 &&
	    (int32_t)(utime - wm.last_time) < 0) {
		ewmh_set_demands_attention(c, true);
		return false;
	}

	(void)t;
	ewmh_set_demands_attention(c, false);
	return true;
}

bool ewmh_handle_client_message(xcb_client_message_event_t *ev)
{
	int idx = atoms_lookup(ev->type);
	if (idx < 0)
		return false;

	struct client *c;

	switch (idx) {
	/*
	 * _GTK_SHOW_WINDOW_MENU (SPEC §7.2)
	 * GTK のヘッダバー右クリックはこれを送ってくる。未対応だと CSD アプリで
	 * ウィンドウメニューを開く手段が無くなる。
	 * data32[0]=device id, data32[1]=x, data32[2]=y (ルート座標)。
	 */
	case ATOM_GTK_SHOW_WINDOW_MENU:
		c = client_find(ev->window);
		if (c != NULL)
			menu_open_window_menu(c, (int16_t)ev->data.data32[1],
			                      (int16_t)ev->data.data32[2]);
		return true;

	/*
	 * _NET_MOVERESIZE_WINDOW (EWMH)
	 * data32[0]: bit0-7=gravity, bit8-11=x/y/w/h の指定有無, bit12-13=source
	 * data32[1..4]: x, y, width, height
	 */
	case ATOM_NET_MOVERESIZE_WINDOW: {
		uint32_t fl = ev->data.data32[0];
		uint8_t  grav = (uint8_t)(fl & 0xff);
		struct rect g;

		c = client_find(ev->window);
		if (c == NULL)
			return true;

		g = c->geom;
		if (fl & (1u << 8))  g.x = (int16_t)ev->data.data32[1];
		if (fl & (1u << 9))  g.y = (int16_t)ev->data.data32[2];
		if (fl & (1u << 10)) g.w = (uint16_t)ev->data.data32[3];
		if (fl & (1u << 11)) g.h = (uint16_t)ev->data.data32[4];

		hints_apply(&c->hints, &g.w, &g.h);
		c->geom = g;
		/* gravity 0 は「ウィンドウの win_gravity を使う」の意 (EWMH) */
		if (fl & ((1u << 8) | (1u << 9)))
			icccm_apply_gravity(c, grav ? grav : c->hints.gravity,
			                    0, 0, &c->geom.x, &c->geom.y);
		c->restore = c->geom;
		client_apply_geometry(c);
		client_send_configure(c);
		return true;
	}

	/*
	 * _NET_WM_MOVERESIZE (EWMH)
	 * data32: [0]=x_root [1]=y_root [2]=direction [3]=button [4]=source
	 * direction 0-7 = 8 方向のリサイズ, 8 = 移動,
	 *           9 = キーボードサイズ, 10 = キーボード移動,
	 *           11 = **CANCEL** (§7.2 で必須。CSD アプリが Esc で使う)
	 */
	case ATOM_NET_WM_MOVERESIZE: {
		static const uint8_t dir_edge[8] = {
			EDGE_T | EDGE_L, EDGE_T, EDGE_T | EDGE_R, EDGE_R,
			EDGE_B | EDGE_R, EDGE_B, EDGE_B | EDGE_L, EDGE_L
		};
		uint32_t dir = ev->data.data32[2];
		int16_t rx = (int16_t)ev->data.data32[0];
		int16_t ry = (int16_t)ev->data.data32[1];

		c = client_find(ev->window);
		if (c == NULL)
			return true;

		if (dir == 11) {                       /* CANCEL */
			if (move_active())
				move_end(true);
		} else if (dir <= 7) {
			move_begin(c, DRAG_RESIZE, dir_edge[dir], rx, ry, wm.last_time);
		} else if (dir == 8 || dir == 10) {
			move_begin(c, DRAG_MOVE, 0, rx, ry, wm.last_time);
		} else if (dir == 9) {
			move_begin(c, DRAG_RESIZE, EDGE_B | EDGE_R, rx, ry, wm.last_time);
		}
		return true;
	}

	/* _NET_RESTACK_WINDOW: [0]=source [1]=sibling [2]=detail */
	case ATOM_NET_RESTACK_WINDOW: {
		struct client *sib;

		c = client_find(ev->window);
		if (c == NULL)
			return true;
		sib = client_find((xcb_window_t)ev->data.data32[1]);
		/* 兄弟指定は stack.c に相対順序の API が無いため、
		 * detail に応じた最上位/最下位への移動で近似する。
		 * (兄弟の直上/直下への挿入は Phase 4 で stack.c を拡張する) */
		(void)sib;
		if (ev->data.data32[2] == XCB_STACK_MODE_BELOW)
			stack_lower(c);
		else
			stack_raise(c);
		stack_apply();
		return true;
	}

	/*
	 * _NET_REQUEST_FRAME_EXTENTS
	 * クライアントは **map する前に** 装飾の厚みを問い合わせてくる。
	 * まだ管理下に無いので、種別と Motif ヒントだけ読んで見積もる。
	 * ここを返さないとアプリが初期サイズを誤る。
	 */
	case ATOM_NET_REQUEST_FRAME_EXTENTS: {
		uint32_t ext[4] = { 0, 0, 0, 0 };
		const struct metrics *m = theme_metrics();

		c = client_find(ev->window);
		if (c != NULL) {
			uint16_t l, r, t, b;
			client_frame_offsets(c, &l, &r, &t, &b);
			ext[0] = l; ext[1] = r; ext[2] = t; ext[3] = b;
		} else {
			/* 未管理。既定の装飾 (通常ウィンドウ) を仮定する */
			ext[0] = ext[1] = ext[3] = m->border_sizing;
			ext[2] = (uint32_t)(m->border_sizing + m->caption_h);
		}
		xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, ev->window,
			atoms[ATOM_NET_FRAME_EXTENTS], XCB_ATOM_CARDINAL, 32, 4, ext);
		return true;
	}

	case ATOM_NET_SHOWING_DESKTOP:
		showing_desktop_set(ev->data.data32[0] != 0);
		return true;

	case ATOM_NET_WM_STATE:
		c = client_find(ev->window);
		if (c != NULL)
			handle_net_wm_state(ev, c);
		return true;

	case ATOM_NET_ACTIVE_WINDOW:
		c = client_find(ev->window);
		if (c != NULL) {
			uint32_t source = ev->data.data32[0];
			xcb_timestamp_t time = (xcb_timestamp_t)ev->data.data32[1];

			/* フォーカススティール防止 (§3.6)。拒否した場合は
			 * DEMANDS_ATTENTION に落とすので要求は無視されない。 */
			if (ewmh_allow_activation(c, source, time)) {
				if (c->flags & CF_ICONIC)
					client_deiconify(c);
				stack_raise(c);
				stack_apply();
				focus_set(c, time);
			}
		}
		return true;

	case ATOM_NET_CLOSE_WINDOW:
		c = client_find(ev->window);
		if (c != NULL)
			client_close(c);
		return true;

	case ATOM_NET_CURRENT_DESKTOP:
		/* frame の map/unmap を伴う実切替 (§3.8) */
		desktop_switch(ev->data.data32[0]);
		ewmh_update_desktop_props();
		return true;

	case ATOM_NET_WM_DESKTOP:
		c = client_find(ev->window);
		if (c != NULL) {
			uint32_t d = ev->data.data32[0];
			if (d == WM_ALL_DESKTOPS || d < wm.n_desktops)
				client_set_desktop(c, d);   /* 表示/非表示も伴う */
		}
		return true;

	case ATOM_WM_CHANGE_STATE:
		/* ICCCM §4.1.4: IconicState (=3) への要求のみ扱う */
		c = client_find(ev->window);
		if (c != NULL && ev->data.data32[0] == 3 /* IconicState */)
			client_iconify(c);
		return true;

	default:
		return false;
	}
}

/* ================================================================== *
 * プロパティ読み取りヘルパ
 * ================================================================== */

bool prop_get_card32(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type,
                     uint32_t *out)
{
	xcb_get_property_cookie_t ck =
	    xcb_get_property(wm.conn, 0, w, prop, type, 0, 1);
	xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return false;

	bool ok = false;
	if (r->type == type && r->format == 32 &&
	    xcb_get_property_value_length(r) >= (int)sizeof(uint32_t)) {
		const uint32_t *v = xcb_get_property_value(r);
		*out = v[0];
		ok = true;
	}
	free(r);
	return ok;
}

uint32_t *prop_get_card32_list(xcb_window_t w, xcb_atom_t prop,
                               xcb_atom_t type, uint32_t *len)
{
	*len = 0;

	/* 1 往復目: 総バイト数だけ知る (offset=0, length=0) */
	xcb_get_property_cookie_t ck =
	    xcb_get_property(wm.conn, 0, w, prop, type, 0, 0);
	xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return NULL;
	uint32_t bytes_after = r->bytes_after;
	free(r);
	if (bytes_after == 0)
		return NULL;

	/* 2 往復目: 実データを丸ごと取得 */
	uint32_t words = (bytes_after + 3) / 4;
	ck = xcb_get_property(wm.conn, 0, w, prop, type, 0, words);
	r = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return NULL;

	if (r->type != type || r->format != 32) {
		free(r);
		return NULL;
	}

	int vlen = xcb_get_property_value_length(r);
	uint32_t n = (uint32_t)(vlen / (int)sizeof(uint32_t));
	if (n == 0) {
		free(r);
		return NULL;
	}

	uint32_t *out = malloc((size_t)n * sizeof(uint32_t));
	if (out == NULL) {
		free(r);
		return NULL;
	}
	memcpy(out, xcb_get_property_value(r), (size_t)n * sizeof(uint32_t));
	free(r);

	*len = n;
	return out;
}

size_t prop_get_text(xcb_window_t w, xcb_atom_t prop, char *buf, size_t buflen)
{
	if (buflen == 0)
		return 0;
	buf[0] = '\0';

	/* format=8 の性質上、リクエストの length 単位（4 バイト境界）は
	 * buflen そのままで十分すぎるほどの余裕を持つ */
	uint32_t words = (uint32_t)buflen;
	xcb_get_property_cookie_t ck =
	    xcb_get_property(wm.conn, 0, w, prop, XCB_ATOM_ANY, 0, words);
	xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return 0;

	int vlen = xcb_get_property_value_length(r);
	if (vlen <= 0 || r->format != 8) {
		free(r);
		return 0;
	}
	const char *data = xcb_get_property_value(r);
	size_t out_len = 0;

	if (r->type == atoms[ATOM_UTF8_STRING]) {
		/* そのまま UTF-8 としてコピー。コードポイント境界で安全に切る */
		size_t copy_len = utf8_truncate_len(data, (size_t)vlen, buflen - 1);
		memcpy(buf, data, copy_len);
		out_len = copy_len;
	} else if (r->type == XCB_ATOM_STRING ||
	           r->type == atoms[ATOM_COMPOUND_TEXT]) {
		/* Latin-1 -> UTF-8 (STRING/COMPOUND_TEXT を Latin-1 とみなす簡易変換) */
		for (int i = 0; i < vlen; i++) {
			uint8_t ch = (uint8_t)data[i];
			size_t need = (ch < 0x80) ? 1 : 2;
			if (out_len + need >= buflen) /* NUL 用に 1 バイト残す */
				break;
			if (ch < 0x80) {
				buf[out_len++] = (char)ch;
			} else {
				buf[out_len++] = (char)(0xC0 | (ch >> 6));
				buf[out_len++] = (char)(0x80 | (ch & 0x3F));
			}
		}
	}
	/* 未知の型は空文字列のまま扱う（呼び出し側に誤情報を渡さない） */

	buf[out_len] = '\0';
	free(r);
	return out_len;
}
