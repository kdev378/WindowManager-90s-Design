/*
 * input.c - キー・マウスの grab とキーバインドのディスパッチ (SPEC §6, §2.2.1)
 *
 * 方針:
 *   - keysym <-> keycode の対応表は xcb-keysyms (xcb_key_symbols_t) が持つ。
 *     MappingNotify のたびに作り直す必要がある。
 *   - NumLock/CapsLock/ScrollLock が立っていても既定バインドが効くように、
 *     修飾子の「ノイズ」(NumLock=Mod2, CapsLock=Lock, ScrollLock=動的検出)
 *     の全組み合わせで grab する（§入力仕様: 「ノイズ込みで直積を grab」）。
 *   - マウス側 (Alt+左/右ドラッグ) は struct binding が保持できない
 *     （keysym のみでボタン番号を持たない）ので、ここで直接 grab する。
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

#define XK_MISCELLANY 1
#include <X11/keysymdef.h>

#include <xcb/xcb_keysyms.h>

extern char **environ;

/* ================================================================== *
 * 状態
 * ================================================================== */

static xcb_key_symbols_t *g_keysyms;

/* ノイズ修飾子。NumLock/CapsLock は仕様どおり固定値、ScrollLock だけは
 * キーボードレイアウトによって割り当てが無いことも多いので動的に調べる。 */
static uint16_t g_num_lock_mask  = XCB_MOD_MASK_2;
static uint16_t g_caps_lock_mask = XCB_MOD_MASK_LOCK;
static uint16_t g_scroll_lock_mask;

/* ================================================================== *
 * ノイズ修飾子の動的検出
 * ================================================================== */

/* 指定した keysym が割り当てられている修飾子グループのマスクを返す。
 * どの修飾子にも割り当てられていなければ 0（ScrollLock はこれが普通）。 */
static uint16_t query_lock_mask(xcb_keysym_t sym)
{
	if (!g_keysyms)
		return 0;

	xcb_keycode_t *kcs = xcb_key_symbols_get_keycode(g_keysyms, sym);
	if (!kcs)
		return 0;

	uint16_t mask = 0;
	xcb_get_modifier_mapping_cookie_t cookie = xcb_get_modifier_mapping(wm.conn);
	xcb_get_modifier_mapping_reply_t *reply =
	        xcb_get_modifier_mapping_reply(wm.conn, cookie, NULL);

	if (reply) {
		xcb_keycode_t *map = xcb_get_modifier_mapping_keycodes(reply);
		int per = reply->keycodes_per_modifier;

		/* modifiers は Shift,Lock,Control,Mod1..Mod5 の順で 8 グループ、
		 * 各グループ keycodes_per_modifier 個。マスクはグループ番号を
		 * ビット位置とする (XCB_MOD_MASK_SHIFT=1<<0 ... MOD_5=1<<7)。 */
		for (int group = 0; group < 8 && mask == 0; group++) {
			for (int k = 0; k < per; k++) {
				xcb_keycode_t kc = map[group * per + k];
				if (kc == 0)
					continue;
				for (xcb_keycode_t *sp = kcs; *sp != 0; sp++) {
					if (*sp == kc) {
						mask = (uint16_t)(1u << group);
						break;
					}
				}
				if (mask != 0)
					break;
			}
		}
		free(reply);
	}

	free(kcs);
	return mask;
}

/* NumLock/CapsLock/ScrollLock の全組み合わせ（重複除去済み）を out に詰め、
 * 個数を返す。ScrollLock が未割当のときは組み合わせが縮退して重複するので
 * 除去する（重複した modifiers で xcb_grab_key を 2 回呼ぶと 2 回目が
 * BadAccess になり得るため）。 */
static int compute_noise_masks(uint16_t out[8])
{
	uint16_t bases[3] = { g_num_lock_mask, g_caps_lock_mask, g_scroll_lock_mask };
	int n = 0;

	for (int i = 0; i < 8; i++) {
		uint16_t m = 0;
		if (i & 1) m = (uint16_t)(m | bases[0]);
		if (i & 2) m = (uint16_t)(m | bases[1]);
		if (i & 4) m = (uint16_t)(m | bases[2]);

		bool dup = false;
		for (int j = 0; j < n; j++) {
			if (out[j] == m) {
				dup = true;
				break;
			}
		}
		if (!dup)
			out[n++] = m;
	}
	return n;
}

/* ================================================================== *
 * grab
 * ================================================================== */

static void grab_all_bindings(void)
{
	uint16_t noise[8];
	int n_noise = compute_noise_masks(noise);

	for (uint16_t i = 0; i < wm.cfg.n_bindings; i++) {
		const struct binding *b = &wm.cfg.bindings[i];
		xcb_keycode_t *kcs = xcb_key_symbols_get_keycode(g_keysyms, b->keysym);
		if (!kcs) {
			ERR("input: keysym 0x%x に対応するキーコードがありません",
			    (unsigned)b->keysym);
			continue;
		}
		for (xcb_keycode_t *kc = kcs; *kc != 0; kc++) {
			for (int j = 0; j < n_noise; j++) {
				xcb_grab_key(wm.conn, 1, wm.root,
				             (uint16_t)(b->mods | noise[j]), *kc,
				             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
			}
		}
		free(kcs);
	}

	/* SPEC §6: Alt+左ドラッグ = 移動, Alt+右ドラッグ = リサイズ。
	 * ルートウィンドウに対して grab し、実際のドラッグ処理は
	 * move_begin() 側（event.c からの ButtonPress ディスパッチ）が行う。
	 * owner_events=0: どのウィンドウの上でも常に root（=WM自身）が受け取る。 */
	for (int j = 0; j < n_noise; j++) {
		xcb_grab_button(wm.conn, 0, wm.root, XCB_EVENT_MASK_BUTTON_PRESS,
		                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
		                XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1,
		                (uint16_t)(XCB_MOD_MASK_1 | noise[j]));
		xcb_grab_button(wm.conn, 0, wm.root, XCB_EVENT_MASK_BUTTON_PRESS,
		                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
		                XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_3,
		                (uint16_t)(XCB_MOD_MASK_1 | noise[j]));
	}
}

static void ungrab_all(void)
{
	xcb_ungrab_key(wm.conn, XCB_GRAB_ANY, wm.root, XCB_MOD_MASK_ANY);
	xcb_ungrab_button(wm.conn, XCB_BUTTON_INDEX_ANY, wm.root, XCB_MOD_MASK_ANY);
}

/* ================================================================== *
 * 公開 API
 * ================================================================== */

void input_init(void)
{
	g_keysyms = xcb_key_symbols_alloc(wm.conn);
	if (!g_keysyms) {
		ERR("input: xcb_key_symbols_alloc に失敗しました");
		return;
	}

	g_scroll_lock_mask = query_lock_mask(XK_Scroll_Lock);
	grab_all_bindings();
}

void input_regrab_keys(void)
{
	ungrab_all();

	if (g_keysyms) {
		/*
		 * xcb_refresh_keyboard_mapping() は本来 MappingNotify イベントの
		 * 実体 (xcb_mapping_notify_event_t*) を渡して呼ぶ関数だが、
		 * このヘッダの契約では input_regrab_keys(void) に引数が無く、
		 * event.c から実イベントを受け取る経路が存在しない
		 * （w98wm.h のギャップ。詳細はレポートに記載）。
		 *
		 * xcb-keysyms の実装は event->request のみを見て
		 * XCB_MAPPING_KEYBOARD かどうかを判定し、真であればテーブル
		 * 全体を xcb_get_keyboard_mapping で取り直す（first_keycode/
		 * count はサーバ側の変更範囲のヒントであり、この再取得の
		 * 要否判定には使われない）。そのため request フィールドだけを
		 * 正しく設定した合成イベントで安全かつ正しく「フル更新」を
		 * 起こせる。他のフィールドは未使用なのでゼロで構わない。
		 */
		xcb_mapping_notify_event_t synth;
		memset(&synth, 0, sizeof(synth));
		synth.response_type = XCB_MAPPING_NOTIFY;
		synth.request = XCB_MAPPING_KEYBOARD;
		xcb_refresh_keyboard_mapping(g_keysyms, &synth);
	} else {
		g_keysyms = xcb_key_symbols_alloc(wm.conn);
		if (!g_keysyms) {
			ERR("input: xcb_key_symbols_alloc に失敗しました");
			return;
		}
	}

	g_scroll_lock_mask = query_lock_mask(XK_Scroll_Lock);
	grab_all_bindings();
}

bool input_handle_key(xcb_key_press_event_t *ev)
{
	if (!g_keysyms)
		return false;

	xcb_keysym_t sym = xcb_key_symbols_get_keysym(g_keysyms, ev->detail, 0);
	if (sym == XCB_NO_SYMBOL)
		return false;

	/* ノイズ修飾子を落とし、ポインタボタンのビット (state の上位バイト) も
	 * 落として、config.c が解決した mods とそのまま比較できる形にする。 */
	uint16_t noise = (uint16_t)(g_num_lock_mask | g_caps_lock_mask | g_scroll_lock_mask);
	uint16_t mods = (uint16_t)((ev->state & (uint16_t)~noise) & 0x00FFu);

	for (uint16_t i = 0; i < wm.cfg.n_bindings; i++) {
		const struct binding *b = &wm.cfg.bindings[i];
		if (b->keysym == sym && b->mods == mods) {
			input_run_action(b->action, b->arg, wm.focused);
			return true;
		}
	}
	return false;
}

void input_run_action(uint8_t action, const char *arg, struct client *c)
{
	switch (action) {
	case ACT_CLOSE:
		if (c)
			client_close(c);
		break;

	case ACT_SWITCH_NEXT: {
		/* Phase 4: Win98 風タスクスイッチャ（アイコン列＋中央パネル）は
		 * 未実装。ここでは MRU リストの次点へ直接フォーカスを移すだけの
		 * 最小実装にとどめる（§6 の Alt+Esc 相当の挙動）。 */
		struct client *next = wm.focused ? wm.focused->focus_next : wm.focus_list;
		if (!next)
			next = wm.focus_list;
		if (next)
			focus_set(next, XCB_CURRENT_TIME);
		break;
	}

	case ACT_SWITCH_PREV: {
		/* MRU リストは focus_next の単方向リンクしか持たないため、
		 * 「ひとつ手前」は先頭から辿って探す。Phase 4 のスイッチャ
		 * パネルが実装されたら、そちらでリング操作をきちんと持つ。 */
		struct client *prev = NULL;
		if (wm.focused) {
			for (struct client *p = wm.focus_list; p; p = p->focus_next) {
				if (p->focus_next == wm.focused) {
					prev = p;
					break;
				}
			}
			if (!prev) {
				for (struct client *p = wm.focus_list; p; p = p->focus_next)
					if (!p->focus_next)
						prev = p;
			}
		} else {
			prev = wm.focus_list;
		}
		if (prev)
			focus_set(prev, XCB_CURRENT_TIME);
		break;
	}

	case ACT_MAXIMIZE:
		if (c)
			layout_maximize(c, true, true, (c->states & ST_MAXIMIZED) == 0);
		break;

	case ACT_MINIMIZE:
		if (c)
			client_iconify(c);
		break;

	case ACT_FULLSCREEN:
		if (c)
			layout_fullscreen(c, (c->states & ST_FULLSCREEN) == 0);
		break;

	case ACT_WINDOW_MENU:
		/*
		 * Alt+Space (§4.6)。キーボード起動なのでポインタ座標が無い。
		 * Windows と同じく、タイトルバーの左下（アイコンの真下）に出す。
		 * マウス経由の起動は event.c がポインタ座標を渡す。
		 */
		if (c) {
			uint16_t l, r, t, b;
			client_frame_offsets(c, &l, &r, &t, &b);
			menu_open_window_menu(c,
				(int16_t)(c->geom.x - l),
				(int16_t)(c->geom.y));
		}
		break;

	case ACT_MOVE_KB:
		if (c)
			move_begin(c, DRAG_MOVE, 0, c->geom.x, c->geom.y, XCB_CURRENT_TIME);
		break;

	case ACT_RESIZE_KB:
		/* 右下端を起点にする。矢印キーでのサイズ変更は move_motion() 側
		 * (move.c) の役目。 */
		if (c)
			move_begin(c, DRAG_RESIZE, EDGE_R | EDGE_B,
			          (int16_t)(c->geom.x + c->geom.w),
			          (int16_t)(c->geom.y + c->geom.h), XCB_CURRENT_TIME);
		break;

	case ACT_DESKTOP_NEXT:
		if (wm.n_desktops > 0) {
			wm.current_desktop = (wm.current_desktop + 1) % wm.n_desktops;
			ewmh_update_desktop_props();
		}
		break;

	case ACT_DESKTOP_PREV:
		if (wm.n_desktops > 0) {
			wm.current_desktop =
			        (wm.current_desktop + wm.n_desktops - 1) % wm.n_desktops;
			ewmh_update_desktop_props();
		}
		break;

	case ACT_SHOW_DESKTOP:
		/* Phase 4: _NET_SHOWING_DESKTOP のトグル・全ウィンドウの
		 * 一時最小化は taskbar/desktop モジュール待ち。 */
		break;

	case ACT_START_MENU:
		/* Phase 4: Win98 風スタートメニューは未実装。 */
		break;

	case ACT_EXEC:
		if (arg)
			spawn_command(arg);
		break;

	case ACT_QUIT:
		wm.running = false;
		break;

	case ACT_NONE:
	default:
		break;
	}
}

/* ================================================================== *
 * spawn (SPEC §2.2.1)
 * ================================================================== */

void spawn_command(const char *cmd)
{
	if (!cmd || cmd[0] == '\0')
		return;

	posix_spawnattr_t attr;
	if (posix_spawnattr_init(&attr) != 0) {
		ERR("spawn: posix_spawnattr_init に失敗しました");
		return;
	}

	sigset_t empty_mask;
	sigemptyset(&empty_mask);

	/*
	 * WM は SIGCHLD を SIG_IGN にしている（§2.2.1: ゾンビを残さないための
	 * POSIX の規定を利用し、waitpid ループを持たない）。しかし SIG_IGN は
	 * exec を跨いで継承されるため、そのままだと spawn した /bin/sh の
	 * 中で wait() を使うシェルスクリプトが誤動作する。
	 *
	 * SIGPIPE も同じ理由で SETSIGDEF に含める必要がある —— これが
	 * 一見冗長に見えるレビュー指摘の対象点。WM は X 接続の切断検知を
	 * xcb_connection_has_error() で行うため SIGPIPE も SIG_IGN にして
	 * いる（§2.2.1）。この SIG_IGN も exec を跨いで継承されるので、
	 * ここで起動する /bin/sh がパイプライン（例: `foo | head`）を
	 * 実行したとき、head が先に終了して foo が書き込みで EPIPE を
	 * 受け取っても SIGPIPE が無視されたままだと foo が正常終了できず、
	 * シェルパイプライン全体の終了処理が乱れる。そのため「WM が既定から
	 * 変更したシグナルすべて」＝ SIGCHLD と SIGPIPE の両方を
	 * POSIX_SPAWN_SETSIGDEF で子側だけ SIG_DFL に戻す。
	 */
	sigset_t def_mask;
	sigemptyset(&def_mask);
	sigaddset(&def_mask, SIGCHLD);
	sigaddset(&def_mask, SIGPIPE);

	posix_spawnattr_setsigdefault(&attr, &def_mask);
	posix_spawnattr_setsigmask(&attr, &empty_mask);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

	char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)cmd, NULL };

	pid_t pid;
	int rc = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
	if (rc != 0)
		ERR("spawn: '%s' の起動に失敗しました: %s", cmd, strerror(rc));

	/* SIGCHLD は WM 側で SIG_IGN のままなので、子の終了はカーネルが自動
	 * 回収する。ステータスを必要としないので wait() は行わない (§2.2.1)。 */

	posix_spawnattr_destroy(&attr);
}
