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
	 * **1 回の UnmapWindow で UnmapNotify は 2 通届く。**
	 *   - クライアントに選んだ StructureNotify   → event == window == c->win
	 *   - フレームに選んだ SubstructureNotify    → event == c->frame
	 * 両方を数えると、下の unmap_pending（1 回の unmap につき 1 加算）が
	 * 足りず、2 通目が「クライアント自身による withdraw」と誤判定されて
	 * ウィンドウが unmanage される。最小化すると窓が消える、という形で出た。
	 *
	 * フレーム経由の 1 通だけを正とする。フレームがまだ無い（adopt 前）
	 * 場合のみ StructureNotify 側を受ける。
	 */
	if (c->frame != XCB_WINDOW_NONE && e->event != c->frame)
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

/*
 * 装飾の有無を再判定して反映する。
 *
 * client_decides_decoration() は「装飾すべきか」を返すだけで
 * CF_DECORATED を書き換えない。戻り値を捨てると、種別や Motif ヒントが
 * 後から変わってもフラグが古いままになり、_NET_FRAME_EXTENTS も
 * ずれ続ける（実際にそのバグを踏んでいた）。
 */
static void refresh_decoration(struct client *c)
{
	uint32_t before = c->flags & CF_DECORATED;

	c->flags &= ~(uint32_t)CF_DECORATED;
	if (client_decides_decoration(c))
		c->flags |= CF_DECORATED;

	client_apply_geometry(c);
	client_update_frame_extents(c);
	if ((c->flags & CF_DECORATED) != before || (c->flags & CF_DECORATED))
		deco_invalidate(c);
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
		refresh_decoration(c);
		stack_apply();
	} else if (e->atom == atoms[ATOM_GTK_FRAME_EXTENTS]) {
		csd_state_changed(c);          /* §7.2 遷移のたびに読み直す */
	} else if (e->atom == atoms[ATOM_MOTIF_WM_HINTS]) {
		motif_update(c);
		refresh_decoration(c);
	} else if (e->atom == atoms[ATOM_NET_WM_ICON]) {
		icon_update(c);                /* §4.4.1 */
		deco_invalidate(c);
	} else if (e->atom == atoms[ATOM_NET_WM_SYNC_REQUEST_COUNTER]) {
		sync_property_changed(c);      /* §7.3 カウンタ差し替え */
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
	enum frame_part part;
	static xcb_timestamp_t last_click;
	static xcb_window_t    last_click_win;
	static uint8_t         last_click_part;
	bool dbl;

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

	/* Alt+左 = 移動、Alt+右 = リサイズ (SPEC §6)。装飾の有無によらず効く */
	if (e->state & XCB_MOD_MASK_1) {
		if (e->detail == XCB_BUTTON_INDEX_1)
			move_begin(c, DRAG_MOVE, 0, e->root_x, e->root_y, e->time);
		else if (e->detail == XCB_BUTTON_INDEX_3)
			move_begin(c, DRAG_RESIZE, EDGE_R | EDGE_B,
			           e->root_x, e->root_y, e->time);
		xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, e->time);
		return;
	}

	/* frame 相対座標で部位を判定する。サブウィンドウは作らない (§3.1) */
	part = deco_hit_test(c, e->event_x, e->event_y);

	/* ダブルクリック判定 (500ms 以内・同じ部位・同じウィンドウ) */
	dbl = (e->time - last_click < 500 && last_click_win == c->frame &&
	       last_click_part == part);
	last_click = e->time;
	last_click_win = c->frame;
	last_click_part = (uint8_t)part;

	if (e->detail == XCB_BUTTON_INDEX_1) {
		switch (part) {
		case PART_ICON:
			/* アイコン左クリック = システムメニュー、
			 * ダブルクリック = 閉じる (SPEC §4.6)。
			 * タイトルバー本体の左クリックはドラッグ移動なので、
			 * メニューをそこに割り当ててはならない。 */
			if (dbl)
				client_close(c);
			else
				menu_open_window_menu(c, e->root_x, e->root_y);
			break;
		case PART_TITLE:
			if (dbl) {
				bool max = (c->states & ST_MAXIMIZED) == ST_MAXIMIZED;
				layout_maximize(c, true, true, !max);
			} else {
				move_begin(c, DRAG_MOVE, 0, e->root_x, e->root_y, e->time);
			}
			break;
		case PART_BTN_MIN:
		case PART_BTN_MAX:
		case PART_BTN_CLOSE:
			/* 押下表示のみ。実行は ButtonRelease（押したまま外に出たら取消） */
			c->press_part = (uint8_t)part;
			c->hover_part = (uint8_t)part;
			deco_draw(c, NULL);
			break;
		case PART_BORDER_N: case PART_BORDER_S:
		case PART_BORDER_E: case PART_BORDER_W:
		case PART_BORDER_NE: case PART_BORDER_NW:
		case PART_BORDER_SE: case PART_BORDER_SW:
			move_begin(c, DRAG_RESIZE, deco_part_edge(part),
			           e->root_x, e->root_y, e->time);
			break;
		default:
			break;
		}
	} else if (e->detail == XCB_BUTTON_INDEX_3 && part == PART_TITLE) {
		menu_open_window_menu(c, e->root_x, e->root_y);
	}

	xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, e->time);
}

static void on_button_release(xcb_button_release_event_t *e)
{
	struct client *c;
	enum frame_part part;
	uint8_t pressed;

	if (move_active()) {
		move_end(false);
		return;
	}

	c = client_find_by_frame(e->event);
	if (!c || c->press_part == PART_NONE)
		return;

	pressed = c->press_part;
	c->press_part = PART_NONE;
	part = deco_hit_test(c, e->event_x, e->event_y);
	deco_draw(c, NULL);

	/* 押した部位の上で離した時だけ実行する（Windows の挙動） */
	if (part != (enum frame_part)pressed)
		return;

	switch (pressed) {
	case PART_BTN_MIN:
		client_iconify(c);
		break;
	case PART_BTN_MAX: {
		bool max = (c->states & ST_MAXIMIZED) == ST_MAXIMIZED;
		layout_maximize(c, true, true, !max);
		break;
	}
	case PART_BTN_CLOSE:
		client_close(c);
		break;
	default:
		break;
	}
}

static void on_motion(xcb_motion_notify_event_t *e)
{
	struct client *c;
	enum frame_part part;

	if (move_active()) {
		move_motion(e->root_x, e->root_y);
		return;
	}

	c = client_find_by_frame(e->event);
	if (!c || !(c->flags & CF_DECORATED))
		return;

	part = deco_hit_test(c, e->event_x, e->event_y);
	if ((uint8_t)part == c->hover_part)
		return;                       /* 変化が無ければ何も送らない */

	c->hover_part = (uint8_t)part;
	deco_set_cursor(c, part);
	if (c->press_part != PART_NONE)
		deco_draw(c, NULL);           /* 押下表示の付け外し */
}

/*
 * Expose (SPEC §4.4)
 *
 * X サーバは 1 つの損傷領域を**複数の矩形に分割して**送り、count は
 * 「この損傷に対する残りのイベント数」を表す。したがって count==0 の
 * 1 通だけを描くと、それ以前の矩形が描かれないまま失われる
 * （実際に、最初のマップでキャプションが描かれない不具合になった）。
 *
 * 正しくは、count が 0 になるまで矩形の外接矩形を貯め、
 * 揃った時点で 1 回だけ描く。ドラッグ中の再描画量も抑えられる。
 */
static void on_expose(xcb_expose_event_t *e)
{
	static xcb_window_t acc_win;
	static int32_t x0, y0, x1, y1;
	static bool acc_valid;
	struct client *c;
	xcb_rectangle_t clip;

	if (acc_valid && acc_win != e->window) {
		/* 別ウィンドウの Expose が割り込んだ。貯めていた分を先に描く */
		struct client *prev = client_find_by_frame(acc_win);
		if (prev && (prev->flags & CF_DECORATED)) {
			clip.x = (int16_t)x0;
			clip.y = (int16_t)y0;
			clip.width  = (uint16_t)(x1 - x0);
			clip.height = (uint16_t)(y1 - y0);
			deco_draw(prev, &clip);
		}
		acc_valid = false;
	}

	if (!acc_valid) {
		acc_win = e->window;
		x0 = e->x;                 y0 = e->y;
		x1 = e->x + e->width;      y1 = e->y + e->height;
		acc_valid = true;
	} else {
		if (e->x < x0) x0 = e->x;
		if (e->y < y0) y0 = e->y;
		if (e->x + e->width  > x1) x1 = e->x + e->width;
		if (e->y + e->height > y1) y1 = e->y + e->height;
	}

	if (e->count != 0)
		return;                    /* まだ続く。貯めるだけ */

	acc_valid = false;
	c = client_find_by_frame(e->window);
	if (!c || !(c->flags & CF_DECORATED))
		return;

	clip.x = (int16_t)x0;
	clip.y = (int16_t)y0;
	clip.width  = (uint16_t)(x1 - x0);
	clip.height = (uint16_t)(y1 - y0);
	deco_draw(c, &clip);
}

static void on_leave_notify(xcb_leave_notify_event_t *e)
{
	struct client *c = client_find_by_frame(e->event);

	if (!c || c->hover_part == PART_NONE)
		return;
	c->hover_part = PART_NONE;
	if (c->press_part != PART_NONE)
		deco_draw(c, NULL);
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

	/* メニューが開いている間はポインタとキーボードを掴んでいる (§4.6)。
	 * 先に回し、消費されなければ通常の処理へ落とす。 */
	if (menu_active() && menu_handle_event(ev))
		return;

	/* XSync のアラーム (SPEC §7.3) と Shape (§5.3) */
	if (sync_handle_event(ev))
		return;
	if (shape_handle_event(ev))
		return;

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
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
		/* 順序: ping の応答 → 起動通知 → EWMH 一般 */
		if (ping_handle_reply(cm))
			break;
		if (startup_handle_message(cm))
			break;
		ewmh_handle_client_message(cm);
		break;
	}
	case XCB_BUTTON_PRESS:
		on_button_press((xcb_button_press_event_t *)ev);
		break;
	case XCB_BUTTON_RELEASE:
		on_button_release((xcb_button_release_event_t *)ev);
		break;
	case XCB_MOTION_NOTIFY:
		on_motion((xcb_motion_notify_event_t *)ev);
		break;
	case XCB_KEY_PRESS:
		input_handle_key((xcb_key_press_event_t *)ev);
		break;
	case XCB_ENTER_NOTIFY:
		on_enter_notify((xcb_enter_notify_event_t *)ev);
		break;
	case XCB_FOCUS_IN:
	case XCB_FOCUS_OUT: {
		/* キャプションの配色がアクティブ/非アクティブで変わる (§4.4) */
		struct client *fc = client_find(((xcb_focus_in_event_t *)ev)->event);
		if (fc && (fc->flags & CF_DECORATED))
			deco_draw(fc, NULL);
		break;
	}
	case XCB_MAPPING_NOTIFY:
		input_regrab_keys();
		break;
	case XCB_SELECTION_CLEAR:
		/* 別の WM がセレクションを奪った。譲って終了する (ICCCM §4.3) */
		LOG("マネージャセレクションを奪われた。終了する");
		wm.running = false;
		break;
	case XCB_EXPOSE:
		on_expose((xcb_expose_event_t *)ev);
		break;
	case XCB_LEAVE_NOTIFY:
		on_leave_notify((xcb_leave_notify_event_t *)ev);
		break;
	default:
		break;
	}
}
