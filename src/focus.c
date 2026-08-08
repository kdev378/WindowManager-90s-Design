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
	if (old && old != c) old->states &= ~ST_FOCUSED;
	c->states |= ST_FOCUSED;
	wm.focused = c;

	if (old && old != c) ewmh_set_wm_state(old);
	ewmh_set_wm_state(c);
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
