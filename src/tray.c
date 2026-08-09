/*
 * tray.c - システムトレイ (SPEC §4.8、XEmbed)
 *
 * _NET_SYSTEM_TRAY_S<n> のマネージャセレクションを取り、
 * REQUEST_DOCK してきたアイコンを自分のコンテナへ reparent する。
 *
 * ★ ARGB の罠 (SPEC §4.8)
 *   最近の常駐アプリは 32bit ARGB ビジュアルのトレイアイコンを使う。
 *   コンポジタが無い環境ではアルファが解決されず背景が黒くなる。
 *   _NET_SYSTEM_TRAY_VISUAL に **24bit（画面既定）のビジュアル**を明示し、
 *   埋め込む前にスロットを face 色で塗ることで、アプリ側がその色を前提に
 *   描いてくれる。完全ではないが実用上これが定石。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

#define TRAY_MAX_ICONS  32
#define TRAY_SPACING     2
#define TRAY_MARGIN      3

/* XEmbed のメッセージ (XEMBED 仕様) */
#define XEMBED_EMBEDDED_NOTIFY  0
#define XEMBED_MAPPED           (1u << 0)   /* _XEMBED_INFO の flags bit0 */

/* _NET_SYSTEM_TRAY_OPCODE の data32[1] */
#define SYSTEM_TRAY_REQUEST_DOCK 0

static struct {
	xcb_window_t owner;        /* セレクション所有 + アイコンのコンテナ */
	xcb_atom_t   sel;
	bool         active;       /* セレクションを取れたか */
	int16_t      x, y;
	uint16_t     h;
	xcb_window_t icon[TRAY_MAX_ICONS];
	bool         mapped[TRAY_MAX_ICONS];
	uint16_t     n;
} tr;

static uint16_t icon_px(void)
{
	const struct metrics *m = theme_metrics();
	return m->icon_size;
}

/* ------------------------------------------------------------------ *
 * アイコン一覧の操作
 * ------------------------------------------------------------------ */
static int find_icon(xcb_window_t w)
{
	uint16_t i;
	for (i = 0; i < tr.n; i++)
		if (tr.icon[i] == w)
			return (int)i;
	return -1;
}

static void remove_icon(int idx)
{
	if (idx < 0 || (uint16_t)idx >= tr.n)
		return;
	/*
	 * save-set から外す (§3.1.1)。既に破棄された窓に対しては
	 * BadWindow になるが、unchecked なので無視されるだけでよい (§2.2.2)。
	 * 外し忘れると、save-set にゴミが溜まったまま WM が生き続ける。
	 */
	xcb_change_save_set(wm.conn, XCB_SET_MODE_DELETE, tr.icon[idx]);
	memmove(&tr.icon[idx], &tr.icon[idx + 1],
	        (size_t)(tr.n - idx - 1) * sizeof tr.icon[0]);
	memmove(&tr.mapped[idx], &tr.mapped[idx + 1],
	        (size_t)(tr.n - idx - 1) * sizeof tr.mapped[0]);
	tr.n--;
	taskbar_update();
}

/*
 * _XEMBED_INFO (2 CARD32: version, flags)。
 * flags の bit0 が XEMBED_MAPPED。立っている時だけ map する。
 * 最初は隠れていて後から現れるアイコンがこれに依存する。
 */
static void apply_xembed_info(uint16_t i)
{
	uint32_t *v;
	uint32_t len = 0;
	bool want = true;

	v = prop_get_card32_list(tr.icon[i], atoms[ATOM_XEMBED_INFO],
	                         atoms[ATOM_XEMBED_INFO], &len);
	if (v != NULL && len >= 2)
		want = (v[1] & XEMBED_MAPPED) != 0;
	free(v);

	if (want && !tr.mapped[i]) {
		xcb_map_window(wm.conn, tr.icon[i]);
		tr.mapped[i] = true;
	} else if (!want && tr.mapped[i]) {
		xcb_unmap_window(wm.conn, tr.icon[i]);
		tr.mapped[i] = false;
	}
}

static void send_embedded_notify(xcb_window_t icon)
{
	xcb_client_message_event_t ev;

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = icon;
	ev.format = 32;
	ev.type = atoms[ATOM_XEMBED];
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = XEMBED_EMBEDDED_NOTIFY;
	ev.data.data32[2] = 0;
	ev.data.data32[3] = tr.owner;
	ev.data.data32[4] = 0;   /* version */
	xcb_send_event(wm.conn, 0, icon, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void dock_icon(xcb_window_t w)
{
	uint32_t vals[1];

	if (!tr.active || w == XCB_WINDOW_NONE)
		return;
	if (find_icon(w) >= 0)
		return;
	if (tr.n >= TRAY_MAX_ICONS) {
		LOG("トレイの上限 (%d) を超えた。無視する", TRAY_MAX_ICONS);
		return;
	}

	/* アイコンは消えやすい。以降のリクエストは全てエラー許容 (§2.2.2) */
	vals[0] = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(wm.conn, w, XCB_CW_EVENT_MASK, vals);

	/*
	 * ★ save-set への登録 (§3.1.1)。**フレームと同じ理由でトレイにも要る。**
	 *
	 * DestroyWindow は所有者に関係なく**サブウィンドウを全て道連れにする**。
	 * トレイのコンテナは WM の資源なので、WM が死ねば（クリーンな終了でも
	 * クラッシュでも）サーバはこれを破棄し、その子である他プロセスの
	 * トレイアイコンまで一緒に消す。常駐アプリ側から見ると
	 * 「WM を再起動したらアイコンが二度と戻らない」という壊れ方をする。
	 *
	 * save-set に入れておけば、サーバは破棄の前にアイコンをルートへ
	 * 戻してくれる。これはコンテナを reparent 親として使う以上、
	 * **必須**であって最適化ではない。
	 */
	xcb_change_save_set(wm.conn, XCB_SET_MODE_INSERT, w);

	xcb_reparent_window(wm.conn, w, tr.owner, 0, 0);
	{
		uint32_t g[2];
		g[0] = icon_px();
		g[1] = icon_px();
		xcb_configure_window(wm.conn, w,
			XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, g);
	}
	send_embedded_notify(w);

	tr.icon[tr.n] = w;
	tr.mapped[tr.n] = false;
	tr.n++;
	apply_xembed_info((uint16_t)(tr.n - 1));
	taskbar_update();
	LOG("トレイにアイコンを追加 (0x%x, 計 %u)", w, tr.n);
}

/* ------------------------------------------------------------------ *
 * 初期化
 * ------------------------------------------------------------------ */
void tray_init(void)
{
	char name[40];
	xcb_intern_atom_reply_t *ar;
	xcb_get_selection_owner_reply_t *owner;
	uint32_t vals[3];
	uint32_t orient = 0;                 /* 0 = 横 */
	xcb_visualid_t visual = wm.visual;   /* 24bit を明示（ARGB の罠。§4.8） */
	xcb_client_message_event_t ev;

	if (!wm.cfg.taskbar || !wm.cfg.tray)
		return;

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", wm.screen_num);
	ar = xcb_intern_atom_reply(wm.conn,
		xcb_intern_atom(wm.conn, 0, (uint16_t)strlen(name), name), NULL);
	if (ar == NULL)
		return;
	tr.sel = ar->atom;
	free(ar);

	owner = xcb_get_selection_owner_reply(wm.conn,
		xcb_get_selection_owner(wm.conn, tr.sel), NULL);
	if (owner != NULL && owner->owner != XCB_WINDOW_NONE) {
		free(owner);
		ERR("別のシステムトレイが動作している。トレイ機能は無効にする");
		return;
	}
	free(owner);

	tr.owner = xcb_generate_id(wm.conn);
	/* 値はビット値の昇順: BACK_PIXEL(2) < OVERRIDE_REDIRECT(512) < EVENT_MASK(2048) */
	vals[0] = wm.cfg.color[THEME_FACE];
	vals[1] = 1;
	vals[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, tr.owner, wm.root,
	                  -100, -100, 1, (uint16_t)icon_px(), 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
	                  XCB_CW_EVENT_MASK, vals);

	/* 名前を付けておく。xwininfo で追えると調査とテストが楽になる */
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tr.owner,
		atoms[ATOM_NET_WM_NAME], atoms[ATOM_UTF8_STRING], 8,
		(uint32_t)strlen(WM_DISPLAY_NAME " tray"), WM_DISPLAY_NAME " tray");

	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tr.owner,
		atoms[ATOM_NET_SYSTEM_TRAY_ORIENTATION], XCB_ATOM_CARDINAL, 32, 1, &orient);
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, tr.owner,
		atoms[ATOM_NET_SYSTEM_TRAY_VISUAL], XCB_ATOM_VISUALID, 32, 1, &visual);

	xcb_set_selection_owner(wm.conn, tr.owner, tr.sel, XCB_CURRENT_TIME);

	owner = xcb_get_selection_owner_reply(wm.conn,
		xcb_get_selection_owner(wm.conn, tr.sel), NULL);
	if (owner == NULL || owner->owner != tr.owner) {
		free(owner);
		xcb_destroy_window(wm.conn, tr.owner);
		tr.owner = XCB_WINDOW_NONE;
		ERR("トレイのセレクションを取得できなかった");
		return;
	}
	free(owner);

	/* MANAGER の告知 (ICCCM §2.8)。トレイクライアントはこれを待っている */
	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = wm.root;
	ev.format = 32;
	ev.type = atoms[ATOM_MANAGER];
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = tr.sel;
	ev.data.data32[2] = tr.owner;
	xcb_send_event(wm.conn, 0, wm.root,
	               XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);

	xcb_map_window(wm.conn, tr.owner);
	tr.active = true;
	LOG("システムトレイを開始 (%s)", name);
}

void tray_fini(void)
{
	if (!tr.active)
		return;
	/* アイコンはクライアントの資源。破棄せずルートへ戻し、save-set から外す */
	while (tr.n > 0) {
		xcb_window_t w = tr.icon[tr.n - 1];

		xcb_reparent_window(wm.conn, w, wm.root, 0, 0);
		xcb_change_save_set(wm.conn, XCB_SET_MODE_DELETE, w);
		tr.n--;
	}
	xcb_destroy_window(wm.conn, tr.owner);
	tr.active = false;
	/*
	 * ここで必ず掃き出す。この後は wm_shutdown() → xcb_disconnect() で、
	 * 送信待ちのまま切断すると上の reparent が届かず、
	 * コンテナの破棄だけがサーバ側で起きてアイコンを道連れにする。
	 */
	xcb_flush(wm.conn);
}

/* ------------------------------------------------------------------ *
 * 配置と描画
 * ------------------------------------------------------------------ */
uint16_t tray_width(void)
{
	if (!tr.active || tr.n == 0)
		return 0;
	return (uint16_t)(TRAY_MARGIN * 2 +
	                  tr.n * icon_px() + (tr.n - 1) * TRAY_SPACING);
}

void tray_place(int16_t x, int16_t y, uint16_t h)
{
	uint32_t vals[4];
	uint16_t i, w = tray_width();

	if (!tr.active)
		return;
	tr.x = x; tr.y = y; tr.h = h;

	if (w == 0) {
		/*
		 * アイコンが 1 つも無くなったら unmap する。
		 * **併せて 1px まで縮める。** unmap しただけだと直前の幅を
		 * 抱えたまま残り、「トレイの幅」を外から見たときに
		 * 実際の中身と食い違う（見た目には出ないが、状態としては嘘）。
		 */
		vals[0] = 1;
		vals[1] = h ? h : 1;
		xcb_configure_window(wm.conn, tr.owner,
			XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
		xcb_unmap_window(wm.conn, tr.owner);
		return;
	}
	/* 値はビット値の昇順: X(1) Y(2) WIDTH(4) HEIGHT(8) */
	vals[0] = (uint32_t)(int32_t)x;
	vals[1] = (uint32_t)(int32_t)y;
	vals[2] = w;
	vals[3] = h;
	xcb_configure_window(wm.conn, tr.owner,
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
	xcb_map_window(wm.conn, tr.owner);

	for (i = 0; i < tr.n; i++) {
		uint32_t g[2];
		g[0] = (uint32_t)(TRAY_MARGIN + i * (icon_px() + TRAY_SPACING));
		g[1] = (uint32_t)((h - icon_px()) / 2);
		xcb_configure_window(wm.conn, tr.icon[i],
			XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, g);
	}
}

void tray_draw(void)
{
	uint16_t w = tray_width();

	if (!tr.active || w == 0)
		return;
	/* 背景を face で塗る（ARGB アイコンがこの色を前提に描く。§4.8） */
	draw_rect(tr.owner, 0, 0, w, tr.h, wm.cfg.color[THEME_FACE]);
	draw_bevel(tr.owner, 0, 0, w, tr.h, BEVEL_SUNKEN);
}

/* ------------------------------------------------------------------ *
 * イベント
 * ------------------------------------------------------------------ */
bool tray_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & 0x7f;

	if (!tr.active)
		return false;

	switch (type) {
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;
		if (e->type != atoms[ATOM_NET_SYSTEM_TRAY_OPCODE])
			return false;
		if (e->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK)
			dock_icon((xcb_window_t)e->data.data32[2]);
		return true;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)ev;
		int i = find_icon(e->window);
		if (i < 0)
			return false;
		remove_icon(i);
		return true;
	}
	case XCB_REPARENT_NOTIFY: {
		xcb_reparent_notify_event_t *e = (xcb_reparent_notify_event_t *)ev;
		int i = find_icon(e->window);
		if (i < 0 || e->parent == tr.owner)
			return false;
		remove_icon(i);            /* 他所へ移された */
		return true;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;
		int i = find_icon(e->window);
		if (i < 0 || e->atom != atoms[ATOM_XEMBED_INFO])
			return false;
		apply_xembed_info((uint16_t)i);
		return true;
	}
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *)ev;
		if (e->window != tr.owner)
			return false;
		if (e->count == 0)
			tray_draw();
		return true;
	}
	case XCB_SELECTION_CLEAR: {
		xcb_selection_clear_event_t *e = (xcb_selection_clear_event_t *)ev;
		/*
		 * WM のマネージャセレクション (event.c が終了に使う) と
		 * 取り違えてはならない。自分のセレクションだけを見る。
		 */
		if (e->selection != tr.sel)
			return false;
		LOG("トレイのセレクションを奪われた");
		tr.active = false;
		tr.n = 0;
		taskbar_update();
		return true;
	}
	default:
		break;
	}
	return false;
}
