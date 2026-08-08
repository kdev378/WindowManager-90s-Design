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

bool ewmh_handle_client_message(xcb_client_message_event_t *ev)
{
	int idx = atoms_lookup(ev->type);
	if (idx < 0)
		return false;

	struct client *c;

	switch (idx) {
	case ATOM_NET_WM_STATE:
		c = client_find(ev->window);
		if (c != NULL)
			handle_net_wm_state(ev, c);
		return true;

	case ATOM_NET_ACTIVE_WINDOW:
		c = client_find(ev->window);
		if (c != NULL) {
			xcb_timestamp_t time = (xcb_timestamp_t)ev->data.data32[1];
			stack_raise(c);
			focus_set(c, time);
		}
		return true;

	case ATOM_NET_CLOSE_WINDOW:
		c = client_find(ev->window);
		if (c != NULL)
			client_close(c);
		return true;

	case ATOM_NET_CURRENT_DESKTOP:
		/* 実際のデスクトップ切替（フレームの map/unmap, §3.8）を行う
		 * ヘルパが w98wm.h に公開されていないため、ここではプロパティの
		 * 更新のみ行う。呼び出し側モジュールが別途切替処理を持つ想定。 */
		if (ev->data.data32[0] < wm.n_desktops)
			wm.current_desktop = ev->data.data32[0];
		ewmh_update_desktop_props();
		return true;

	case ATOM_NET_WM_DESKTOP:
		/* 同上。クライアントの所属デスクトップの記録のみ行う。 */
		c = client_find(ev->window);
		if (c != NULL)
			c->desktop = ev->data.data32[0];
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
