/*
 * event.c - X イベントのディスパッチ
 *
 * 対応: SPEC §2.2.2(エラー処理), §3.1.0, §3.4, §3.8(Unmap カウンタ), §5.3
 */
#include <stdlib.h>
#include <string.h>

#include <xcb/randr.h>

#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * エラー (SPEC §2.2.2)
 *
 * xcb では unchecked リクエストのエラーが response_type == 0 の
 * 「イベント」として遅れて届く。クライアント宛は通常の動作条件なので
 * 握り潰し、自リソース宛は実装バグとして報告する。
 * 一律破棄にはしない（自分のバグを永久に隠蔽するため）。
 * ------------------------------------------------------------------ */
static const char *req_name(uint8_t major)
{
	switch (major) {
	case 1:  return "CreateWindow";
	case 2:  return "ChangeWindowAttributes";
	case 3:  return "GetWindowAttributes";
	case 7:  return "ReparentWindow";
	case 8:  return "MapWindow";
	case 10: return "UnmapWindow";
	case 12: return "ConfigureWindow";
	case 18: return "ChangeProperty";
	case 20: return "GetProperty";
	case 42: return "SetInputFocus";
	default: return "?";
	}
}

void event_handle_error(xcb_generic_error_t *err)
{
	bool ours;

	/*
	 * 自分が作ったリソース（frame / check_win / focus_win）宛かどうか。
	 * frame は client リストから引ける。
	 */
	ours = (err->resource_id == wm.check_win ||
	        err->resource_id == wm.focus_win ||
	        client_find_by_frame(err->resource_id) != NULL);

	if (ours) {
		ERR("自リソースへのエラー: code=%u major=%u(%s) minor=%u res=0x%x seq=%u",
		    err->error_code, err->major_code, req_name(err->major_code),
		    err->minor_code, err->resource_id, err->sequence);
		return;
	}

	/* クライアント宛。消えたウィンドウへのリクエストは通常起きる。 */
	switch (err->error_code) {
	case XCB_WINDOW:      /* BadWindow */
	case XCB_DRAWABLE:    /* BadDrawable */
	case XCB_MATCH:       /* BadMatch */
		LOG("クライアント宛エラーを無視: code=%u major=%u res=0x%x",
		    err->error_code, err->major_code, err->resource_id);
		break;
	default:
		ERR("想定外のエラー: code=%u major=%u(%s) res=0x%x",
		    err->error_code, err->major_code, req_name(err->major_code),
		    err->resource_id);
		break;
	}
}

/* ------------------------------------------------------------------ *
 * 個別ハンドラ
 * ------------------------------------------------------------------ */
static void on_map_request(xcb_map_request_event_t *e)
{
	struct client *c = client_find(e->window);

	if (!c) {
		c = client_manage(e->window, false);
		if (!c) {
			/*
			 * 管理できなかった (上限超過・確保失敗・既に消滅)。
			 * SPEC §2.3.0: 殺さず装飾なしでそのまま map する。
			 * client_manage 側で map 済みなのでここでは何もしない。
			 */
			return;
		}
	}
	client_deiconify(c);

	/*
	 * 新しく開いたウィンドウをアクティブにする。
	 * click-to-focus でも「今開いたウィンドウは能動化する」のが
	 * 通常の挙動であり、これが無いと _NET_ACTIVE_WINDOW が None のままで
	 * キーボード入力の行き先が無い。
	 * フォーカスを取れない種別 (DOCK/SPLASH/ポップアップ類, §5.2.2) は除く。
	 * 起動時の adopt 経路はここを通らない。
	 *
	 * TODO(Phase 3): _NET_WM_USER_TIME によるフォーカススティール防止 (§3.6)。
	 *   古い user_time を持つ自己アクティブ化は DEMANDS_ATTENTION に落とす。
	 */
	if (type_props(c->type)->focusable && !(c->flags & CF_ICONIC))
		focus_set(c, wm.last_time);
}

static void on_configure_request(xcb_configure_request_event_t *e)
{
	struct client *c = client_find(e->window);
	uint32_t vals[7];
	uint16_t mask = 0;
	int n = 0;

	if (!c) {
		/* 未管理のウィンドウは要求をそのまま通す (ICCCM §4.1.5) */
		if (e->value_mask & XCB_CONFIG_WINDOW_X)            { vals[n++] = (uint32_t)e->x;  mask |= XCB_CONFIG_WINDOW_X; }
		if (e->value_mask & XCB_CONFIG_WINDOW_Y)            { vals[n++] = (uint32_t)e->y;  mask |= XCB_CONFIG_WINDOW_Y; }
		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)        { vals[n++] = e->width;        mask |= XCB_CONFIG_WINDOW_WIDTH; }
		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)       { vals[n++] = e->height;       mask |= XCB_CONFIG_WINDOW_HEIGHT; }
		if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) { vals[n++] = e->border_width; mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH; }
		if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)      { vals[n++] = e->sibling;      mask |= XCB_CONFIG_WINDOW_SIBLING; }
		if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)   { vals[n++] = e->stack_mode;   mask |= XCB_CONFIG_WINDOW_STACK_MODE; }
		if (mask)
			xcb_configure_window(wm.conn, e->window, mask, vals);
		return;
	}

	/*
	 * 管理下のウィンドウ。全画面・最大化中はジオメトリ要求を無視し、
	 * ICCCM §4.1.5 に従って synthetic ConfigureNotify を返す。
	 */
	if (c->states & (ST_FULLSCREEN | ST_MAXIMIZED)) {
		client_send_configure(c);
		return;
	}

	if (e->value_mask & XCB_CONFIG_WINDOW_X)
		c->geom.x = e->x;
	if (e->value_mask & XCB_CONFIG_WINDOW_Y)
		c->geom.y = e->y;
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
		c->geom.w = e->width;
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
		c->geom.h = e->height;

	/* グラビティを適用して装飾分を補正 (ICCCM §4.1.5) */
	if (e->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y))
		icccm_apply_gravity(c, c->hints.gravity, c->border_orig, 0,
		                    &c->geom.x, &c->geom.y);

	c->restore = c->geom;
	client_apply_geometry(c);
	client_send_configure(c);

	if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		if (e->stack_mode == XCB_STACK_MODE_ABOVE)
			stack_raise(c);
		else if (e->stack_mode == XCB_STACK_MODE_BELOW)
			stack_lower(c);
	}
}

static void on_unmap_notify(xcb_unmap_notify_event_t *e)
{
	struct client *c = client_find(e->window);

	if (!c || e->window != c->win)
		return;

	/*
	 * SPEC §3.8: WM 起因の Unmap（reparent / 最小化 / デスクトップ切替）は
	 * 同一カウンタで無視する。ここを誤ると「ウィンドウが勝手に消える」
	 * という最も追いにくいバグになる。
	 */
	if (c->unmap_pending > 0) {
		c->unmap_pending--;
		return;
	}

	/* クライアント自身によるアンマップ = Withdrawn へ (ICCCM §4.1.4) */
	client_unmanage(c, false);
	stack_apply();
}

static void on_destroy_notify(xcb_destroy_notify_event_t *e)
{
	struct client *c = client_find(e->window);

	if (c && e->window == c->win) {
		c->flags |= CF_DESTROYED;   /* 以後このウィンドウへ一切送らない */
		client_unmanage(c, true);
		stack_apply();
	}
}

static void on_property_notify(xcb_property_notify_event_t *e)
{
	struct client *c;

	if (e->window == wm.root)
		return;

	c = client_find(e->window);
	if (!c || e->window != c->win)
		return;

	if (e->atom == atoms[ATOM_NET_WM_NAME] || e->atom == XCB_ATOM_WM_NAME) {
		icccm_update_title(c);
	} else if (e->atom == XCB_ATOM_WM_NORMAL_HINTS) {
		icccm_update_size_hints(c);
		ewmh_set_allowed_actions(c);
	} else if (e->atom == XCB_ATOM_WM_HINTS) {
		icccm_update_wm_hints(c);
	} else if (e->atom == XCB_ATOM_WM_TRANSIENT_FOR) {
		icccm_update_transient(c);
		stack_apply();
	} else if (e->atom == atoms[ATOM_NET_WM_WINDOW_TYPE]) {
		icccm_update_window_type(c);
		client_decides_decoration(c);
		stack_apply();
	} else if (e->atom == atoms[ATOM_GTK_FRAME_EXTENTS]) {
		icccm_update_gtk_extents(c);
		client_decides_decoration(c);
		client_apply_geometry(c);
	} else if (e->atom == atoms[ATOM_NET_WM_STRUT] ||
	           e->atom == atoms[ATOM_NET_WM_STRUT_PARTIAL]) {
		layout_update_workareas();
	} else if (e->atom == atoms[ATOM_NET_WM_USER_TIME]) {
		uint32_t t;
		if (prop_get_card32(c->win, atoms[ATOM_NET_WM_USER_TIME],
		                    XCB_ATOM_CARDINAL, &t))
			c->user_time = t;
	}
}

static void on_button_press(xcb_button_press_event_t *e)
{
	struct client *c = client_find_by_frame(e->event);

	if (!c)
		c = client_find(e->child);
	if (!c) {
		xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, e->time);
		return;
	}

	if (wm.cfg.focus_mode == FOCUS_CLICK)
		focus_set(c, e->time);
	stack_raise(c);
	stack_apply();

	/*
	 * Alt+左 = 移動、Alt+右 = リサイズ (SPEC §6)。
	 * Phase 2 でタイトルバー・ボーダーの当たり判定が入る。
	 */
	if (e->state & XCB_MOD_MASK_1) {
		if (e->detail == XCB_BUTTON_INDEX_1)
			move_begin(c, DRAG_MOVE, 0, e->root_x, e->root_y, e->time);
		else if (e->detail == XCB_BUTTON_INDEX_3)
			move_begin(c, DRAG_RESIZE, EDGE_R | EDGE_B,
			           e->root_x, e->root_y, e->time);
	}
	xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, e->time);
}

static void on_enter_notify(xcb_enter_notify_event_t *e)
{
	struct client *c;

	if (wm.cfg.focus_mode != FOCUS_SLOPPY)
		return;
	if (e->mode != XCB_NOTIFY_MODE_NORMAL || e->detail == XCB_NOTIFY_DETAIL_INFERIOR)
		return;

	c = client_find_by_frame(e->event);
	if (c && c != wm.focused)
		focus_set(c, e->time);
}

/* ------------------------------------------------------------------ *
 * ディスパッチ
 * ------------------------------------------------------------------ */
void event_dispatch(xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & 0x7f;   /* send_event ビットを落とす */

	/* 直近のイベント時刻を控える。ICCCM は CurrentTime より実時刻を好む */
	switch (type) {
	case XCB_KEY_PRESS:
	case XCB_KEY_RELEASE:
		wm.last_time = ((xcb_key_press_event_t *)ev)->time; break;
	case XCB_BUTTON_PRESS:
	case XCB_BUTTON_RELEASE:
		wm.last_time = ((xcb_button_press_event_t *)ev)->time; break;
	case XCB_MOTION_NOTIFY:
		wm.last_time = ((xcb_motion_notify_event_t *)ev)->time; break;
	case XCB_ENTER_NOTIFY:
		wm.last_time = ((xcb_enter_notify_event_t *)ev)->time; break;
	case XCB_PROPERTY_NOTIFY:
		wm.last_time = ((xcb_property_notify_event_t *)ev)->time; break;
	default: break;
	}

	if (ev->response_type == 0) {
		event_handle_error((xcb_generic_error_t *)ev);
		return;
	}

	/* RandR (SPEC §5.3) */
	if (wm.have_randr &&
	    type == wm.randr_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
		layout_update_monitors();
		ewmh_update_desktop_props();
		return;
	}

	switch (type) {
	case XCB_MAP_REQUEST:
		on_map_request((xcb_map_request_event_t *)ev);
		break;
	case XCB_CONFIGURE_REQUEST:
		on_configure_request((xcb_configure_request_event_t *)ev);
		break;
	case XCB_UNMAP_NOTIFY:
		on_unmap_notify((xcb_unmap_notify_event_t *)ev);
		break;
	case XCB_DESTROY_NOTIFY:
		on_destroy_notify((xcb_destroy_notify_event_t *)ev);
		break;
	case XCB_PROPERTY_NOTIFY:
		on_property_notify((xcb_property_notify_event_t *)ev);
		break;
	case XCB_CLIENT_MESSAGE:
		ewmh_handle_client_message((xcb_client_message_event_t *)ev);
		break;
	case XCB_BUTTON_PRESS:
		on_button_press((xcb_button_press_event_t *)ev);
		break;
	case XCB_BUTTON_RELEASE:
		if (move_active())
			move_end(false);
		break;
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)ev;
		if (move_active())
			move_motion(m->root_x, m->root_y);
		break;
	}
	case XCB_KEY_PRESS:
		input_handle_key((xcb_key_press_event_t *)ev);
		break;
	case XCB_ENTER_NOTIFY:
		on_enter_notify((xcb_enter_notify_event_t *)ev);
		break;
	case XCB_MAPPING_NOTIFY:
		input_regrab_keys();
		break;
	case XCB_SELECTION_CLEAR:
		/* 別の WM がセレクションを奪った。譲って終了する (ICCCM §4.3) */
		LOG("マネージャセレクションを奪われた。終了する");
		wm.running = false;
		break;
	case XCB_EXPOSE:
		/* Phase 2 で装飾を描く */
		break;
	default:
		break;
	}
}
