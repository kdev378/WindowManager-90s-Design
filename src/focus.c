/*
 * focus.c - フォーカス制御 (SPEC §3.6, ICCCM §4.1.7)
 */
#include <string.h>

#include "w98wm.h"

/* WM_TAKE_FOCUS を送る (ICCCM §4.1.7)。ClientMessage は直接
 * SendEvent するのが作法で、event_mask は 0 (NoEventMask) を使う。 */
static void send_take_focus(struct client *c, xcb_timestamp_t time)
{
	xcb_client_message_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = c->win;
	ev.type = atoms[ATOM_WM_PROTOCOLS];
	ev.data.data32[0] = atoms[ATOM_WM_TAKE_FOCUS];
	ev.data.data32[1] = time;

	xcb_send_event(wm.conn, false, c->win, XCB_EVENT_MASK_NO_EVENT,
	               (const char *)&ev);
}

/*
 * キャプションの配色はアクティブ/非アクティブで変わる (§4.4)。
 *
 * ★ これを FocusIn/FocusOut イベントに任せてはいけない。
 *   どのウィンドウにも FOCUS_CHANGE のイベントマスクを選んでいなかったため、
 *   event.c にあった FocusIn ハンドラは**一度も呼ばれていなかった**。
 *   キャプションは Expose のときしか描き直されず、色は「最後に Expose
 *   された時点の状態」で固まっていた（外部テストで発見）。
 *
 *   しかも FocusIn は X の入力フォーカスに従うので、Globally Active
 *   (WM が SetInputFocus を呼ばない) や No Input のクライアントでは
 *   WM の「アクティブ」と一致しない。正はあくまで ST_FOCUSED であり、
 *   それを書き換えた場所で描き直すのが唯一確実な方法である。
 */
static void redraw_caption(struct client *c)
{
	if (c != NULL && (c->flags & CF_DECORATED) && (c->flags & CF_MAPPED))
		deco_draw(c, NULL);
}

/*
 * click-to-focus のためのパッシブ grab (SPEC §3.6)。
 *
 * ★ これが無いと「窓の中身をクリックしてもアクティブにならない」。
 *   フレームは ButtonPress を select しているが、xterm や GTK のように
 *   アプリ自身が ButtonPress を select している窓では、イベントは
 *   アプリに届いてフレームへは伝播しない。WM が受け取れるのは
 *   **タイトルバーと枠のクリックだけ**になる。
 *   event.c には ReplayPointer を呼ぶコードがあったのに grab を
 *   張っていなかったため、ずっとこの状態だった（外部テストで発見。
 *   既存テストは全部 _NET_ACTIVE_WINDOW で切り替えていて踏まなかった）。
 *
 * 方式は dwm と同じ: **非アクティブな窓だけ grab し、アクティブに
 * なったら外す**。アクティブな窓の中のクリックには一切割り込まないので、
 * 日常の操作で ReplayPointer の往復が挟まらない。
 * pointer_mode を SYNC にして、受けたら必ず ReplayPointer で解放する
 * (event.c の on_button_press)。
 */
void focus_grab_buttons(struct client *c, bool grab)
{
	if (c == NULL || (c->flags & CF_DESTROYED))
		return;
	if (wm.cfg.focus_mode != FOCUS_CLICK)
		return;

	if (grab)
		xcb_grab_button(wm.conn, 0, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
		                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
		                XCB_NONE, XCB_NONE,
		                XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
	else
		xcb_ungrab_button(wm.conn, XCB_BUTTON_INDEX_ANY, c->win,
		                  XCB_MOD_MASK_ANY);
}

void focus_set(struct client *c, xcb_timestamp_t time)
{
	if (!c) { focus_none(); return; }

	bool input      = (c->flags & CF_INPUT_HINT) != 0;
	bool take_focus = (c->flags & CF_TAKE_FOCUS) != 0;

	/* ICCCM §4.1.7 の4モデル:
	 *   input && !take_focus  -> Passive
	 *   input &&  take_focus  -> Locally Active
	 *  !input &&  take_focus  -> Globally Active（SetInputFocus は呼ばない。
	 *                             Java/Swing や一部 Qt アプリに必須）
	 *  !input && !take_focus  -> No Input（一切フォーカスを与えない） */
	if (!input && !take_focus)
		return;

	if (input)
		xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_PARENT, c->win, time);

	if (take_focus)
		send_take_focus(c, time);

	struct client *old = wm.focused;
	bool was_focused = (c->states & ST_FOCUSED) != 0;
	if (old && old != c) old->states &= ~ST_FOCUSED;
	c->states |= ST_FOCUSED;
	wm.focused = c;

	if (old && old != c) {
		ewmh_set_wm_state(old);
		redraw_caption(old);
		focus_grab_buttons(old, true);
	}
	ewmh_set_wm_state(c);
	if (!was_focused)
		redraw_caption(c);
	focus_grab_buttons(c, false);
	ewmh_update_active_window();

	focus_mru_promote(c);

	if (wm.cfg.focus_raise)
		stack_raise(c);

	/* 全画面のレイヤ判定はフォーカス保持者が誰かに依存するため、
	 * フォーカス変更のたびに再計算が要る (§3.7.1)。 */
	stack_apply();
}

void focus_none(void)
{
	struct client *old = wm.focused;
	if (old) {
		old->states &= ~ST_FOCUSED;
		ewmh_set_wm_state(old);
		redraw_caption(old);
		focus_grab_buttons(old, true);
	}
	wm.focused = NULL;

	/* PointerRoot には絶対に戻さない（click-to-focus 設定なのに
	 * ポインタ位置で入力先が変わる sloppy focus 的挙動が漏れるため）。
	 * 行き先が無いときは 1x1 の InputOnly ダミーへ逃がす (§3.6)。 */
	xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_PARENT, wm.focus_win,
	                    XCB_CURRENT_TIME);

	ewmh_update_active_window();
	stack_apply();
}

void focus_next_after(struct client *closing)
{
	for (struct client *c = wm.focus_list; c; c = c->focus_next) {
		if (c == closing) continue;
		if (c->flags & CF_ICONIC) continue;
		if (c->desktop != wm.current_desktop &&
		    c->desktop != WM_ALL_DESKTOPS &&
		    !(c->states & ST_STICKY))
			continue;
		if (!type_props(c->type)->focusable) continue;

		focus_set(c, XCB_CURRENT_TIME);
		return;
	}
	focus_none();
}

void focus_mru_remove(struct client *c)
{
	if (!c) return;

	if (wm.focus_list == c) {
		wm.focus_list = c->focus_next;
		c->focus_next = NULL;
		return;
	}
	for (struct client *p = wm.focus_list; p; p = p->focus_next) {
		if (p->focus_next == c) {
			p->focus_next = c->focus_next;
			c->focus_next = NULL;
			return;
		}
	}
}

void focus_mru_promote(struct client *c)
{
	if (!c) return;

	focus_mru_remove(c);
	c->focus_next = wm.focus_list;
	wm.focus_list = c;
}

struct client *focus_mru_first(uint32_t desktop)
{
	for (struct client *c = wm.focus_list; c; c = c->focus_next) {
		if (c->flags & CF_ICONIC) continue;
		if (!type_props(c->type)->focusable) continue;
		if (c->desktop == desktop ||
		    c->desktop == WM_ALL_DESKTOPS ||
		    (c->states & ST_STICKY))
			return c;
	}
	return NULL;
}
