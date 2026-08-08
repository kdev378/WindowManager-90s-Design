/*
 * wm.c - 初期化・マネージャセレクション・既存ウィンドウの adopt
 *
 * 対応: SPEC §5.1(ICCCM §4.3), §5.3(拡張), §3.6(focus_win), PLAN Phase 1-1/1-2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>
#include <xcb/sync.h>

#include "w98wm.h"

struct wm wm;

/* ------------------------------------------------------------------ *
 * 拡張の問い合わせ
 * ------------------------------------------------------------------ */
static void query_extensions(void)
{
	const xcb_query_extension_reply_t *ext;

	/* RandR は必須。無い場合は単一モニタに縮退して動作継続 (SPEC §5.3) */
	ext = xcb_get_extension_data(wm.conn, &xcb_randr_id);
	if (ext && ext->present) {
		wm.have_randr = true;
		wm.randr_base = ext->first_event;
		xcb_randr_select_input(wm.conn, wm.root,
			XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
			XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
			XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
	} else {
		wm.have_randr = false;
		ERR("RandR が無い。マルチモニタ非対応で続行する (SPEC §5.3)");
	}

	/* XSync は _NET_WM_SYNC_REQUEST に必須 (SPEC §7.3) */
	ext = xcb_get_extension_data(wm.conn, &xcb_sync_id);
	if (ext && ext->present) {
		wm.have_sync = true;
		wm.sync_base = ext->first_event;
	} else {
		wm.have_sync = false;
		ERR("XSync が無い。リサイズの同期を行わない (SPEC §7.3)");
	}
}

/* ------------------------------------------------------------------ *
 * マネージャセレクション WM_S<n> (ICCCM §4.3)
 * ------------------------------------------------------------------ */
static xcb_window_t sel_owner_win;
static xcb_atom_t   sel_atom;

bool wm_acquire_selection(bool replace)
{
	char name[32];
	xcb_intern_atom_reply_t *ar;
	xcb_get_selection_owner_reply_t *owner;
	xcb_window_t prev = XCB_WINDOW_NONE;
	uint32_t vals[2];
	xcb_client_message_event_t ev;

	snprintf(name, sizeof name, "WM_S%d", wm.screen_num);
	ar = xcb_intern_atom_reply(wm.conn,
		xcb_intern_atom(wm.conn, 0, (uint16_t)strlen(name), name), NULL);
	if (!ar)
		return false;
	sel_atom = ar->atom;
	free(ar);

	owner = xcb_get_selection_owner_reply(wm.conn,
		xcb_get_selection_owner(wm.conn, sel_atom), NULL);
	if (owner) {
		prev = owner->owner;
		free(owner);
	}
	if (prev != XCB_WINDOW_NONE && !replace) {
		ERR("既に別のウィンドウマネージャが動作している (--replace で置換)");
		return false;
	}

	/* セレクション所有用の 1x1 InputOnly */
	sel_owner_win = xcb_generate_id(wm.conn);
	vals[0] = 1;  /* override_redirect */
	xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, sel_owner_win, wm.root,
		-1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT, vals);

	if (prev != XCB_WINDOW_NONE) {
		/* 前の WM の消滅を検知するため StructureNotify を選ぶ */
		vals[0] = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
		xcb_change_window_attributes(wm.conn, prev, XCB_CW_EVENT_MASK, vals);
		xcb_flush(wm.conn);
	}

	xcb_set_selection_owner(wm.conn, sel_owner_win, sel_atom, XCB_CURRENT_TIME);

	owner = xcb_get_selection_owner_reply(wm.conn,
		xcb_get_selection_owner(wm.conn, sel_atom), NULL);
	if (!owner || owner->owner != sel_owner_win) {
		free(owner);
		ERR("マネージャセレクションを取得できなかった");
		return false;
	}
	free(owner);

	/* 他のクライアントへ MANAGER を通知 (ICCCM §2.8) */
	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = wm.root;
	ev.format = 32;
	ev.type = atoms[ATOM_MANAGER];
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = sel_atom;
	ev.data.data32[2] = sel_owner_win;
	xcb_send_event(wm.conn, 0, wm.root,
		XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
	return true;
}

/* ------------------------------------------------------------------ *
 * 既存ウィンドウの取り込み (PLAN Phase 1-2)
 *
 * 走査条件は「Viewable」または「WM_STATE を持つ」。後者を落とすと、
 * 前の WM が Iconic にしたウィンドウ (unmapped + WM_STATE=Iconic) を
 * 取りこぼして復帰不能になる。
 * ------------------------------------------------------------------ */
void wm_scan_existing(void)
{
	xcb_query_tree_reply_t *tree;
	xcb_window_t *kids;
	int n, i;

	tree = xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
	if (!tree)
		return;
	kids = xcb_query_tree_children(tree);
	n = xcb_query_tree_children_length(tree);

	for (i = 0; i < n; i++) {
		xcb_get_window_attributes_reply_t *at;
		bool viewable, has_state = false;
		uint32_t dummy;

		at = xcb_get_window_attributes_reply(wm.conn,
			xcb_get_window_attributes(wm.conn, kids[i]), NULL);
		if (!at)
			continue;                       /* 既に消えている */
		if (at->override_redirect) {
			free(at);
			continue;
		}
		viewable = (at->map_state == XCB_MAP_STATE_VIEWABLE);
		free(at);

		if (!viewable)
			has_state = prop_get_card32(kids[i], atoms[ATOM_WM_STATE],
			                            atoms[ATOM_WM_STATE], &dummy);
		if (viewable || has_state)
			client_manage(kids[i], true);
	}
	free(tree);
	stack_apply();
}

/* ------------------------------------------------------------------ *
 * 初期化
 * ------------------------------------------------------------------ */
static bool take_root(void)
{
	/*
	 * SubstructureRedirect を取れるのは 1 クライアントのみ。
	 * checked で発行し、BadAccess なら他の WM が居る。
	 */
	uint32_t mask =
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_PROPERTY_CHANGE |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_ENTER_WINDOW;
	xcb_generic_error_t *err;

	err = xcb_request_check(wm.conn,
		xcb_change_window_attributes_checked(wm.conn, wm.root,
			XCB_CW_EVENT_MASK, &mask));
	if (err) {
		free(err);
		return false;
	}
	return true;
}

bool wm_init(int argc, char **argv)
{
	const xcb_setup_t *setup;
	xcb_screen_iterator_t it;
	int i;
	bool replace = false;
	uint32_t vals[1];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--replace") == 0)
			replace = true;
	}

	wm.conn = xcb_connect(NULL, &wm.screen_num);
	if (xcb_connection_has_error(wm.conn)) {
		ERR("X サーバに接続できない");
		return false;
	}

	setup = xcb_get_setup(wm.conn);
	it = xcb_setup_roots_iterator(setup);
	for (i = 0; i < wm.screen_num; i++)
		xcb_screen_next(&it);
	wm.screen = it.data;
	wm.root   = wm.screen->root;
	wm.visual = wm.screen->root_visual;
	wm.depth  = wm.screen->root_depth;

	atoms_init(wm.conn);
	query_extensions();

	if (!wm_acquire_selection(replace))
		return false;
	if (!take_root()) {
		ERR("SubstructureRedirect を取得できない。他の WM が動作している");
		return false;
	}

	/*
	 * フォーカスの受け皿 (SPEC §3.6)。
	 * 行き先が無いときに PointerRoot へ戻すと、click-to-focus 設定でも
	 * ポインタ位置で入力先が変わる挙動が漏れる。1x1 InputOnly に逃がす。
	 */
	wm.focus_win = xcb_generate_id(wm.conn);
	vals[0] = 1;
	xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, wm.focus_win, wm.root,
		-1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT, vals);
	xcb_map_window(wm.conn, wm.focus_win);

	wm.n_desktops = wm.cfg.desktops ? wm.cfg.desktops : 1;
	wm.current_desktop = 0;
	wm.running = true;

	ewmh_init();
	layout_update_monitors();
	ewmh_update_desktop_props();
	input_init();
	focus_none();

	wm_scan_existing();
	xcb_flush(wm.conn);

	LOG("起動完了 (%s, %d モニタ, RandR:%s XSync:%s)",
	    WM_OS_NAME, wm.n_monitors,
	    wm.have_randr ? "有" : "無", wm.have_sync ? "有" : "無");
	return true;
}

/* ------------------------------------------------------------------ *
 * 終了処理
 *
 * 正常終了時のみ走る。異常終了 (kill -9 等) ではセーブセット
 * (SPEC §3.1.1) がウィンドウを救う。両方が必要。
 * ------------------------------------------------------------------ */
void wm_shutdown(void)
{
	struct client *c, *next;

	for (c = wm.stack_bottom; c; c = next) {
		next = c->next;
		client_unmanage(c, false);
	}
	if (wm.focus_win != XCB_WINDOW_NONE)
		xcb_destroy_window(wm.conn, wm.focus_win);
	if (wm.check_win != XCB_WINDOW_NONE)
		xcb_destroy_window(wm.conn, wm.check_win);
	if (sel_owner_win != XCB_WINDOW_NONE)
		xcb_destroy_window(wm.conn, sel_owner_win);

	xcb_delete_property(wm.conn, wm.root, atoms[ATOM_NET_SUPPORTED]);
	xcb_delete_property(wm.conn, wm.root, atoms[ATOM_NET_CLIENT_LIST]);
	xcb_delete_property(wm.conn, wm.root, atoms[ATOM_NET_CLIENT_LIST_STACKING]);
	xcb_delete_property(wm.conn, wm.root, atoms[ATOM_NET_SUPPORTING_WM_CHECK]);
	xcb_delete_property(wm.conn, wm.root, atoms[ATOM_NET_ACTIVE_WINDOW]);

	xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
	                    XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	xcb_flush(wm.conn);
	config_free(&wm.cfg);
	xcb_disconnect(wm.conn);
}
