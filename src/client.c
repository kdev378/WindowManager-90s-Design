/*
 * client.c - クライアントのライフサイクル
 *
 * 対応する仕様: docs/SPEC.md §3.1, §3.1.0, §3.1.1, §3.2, §3.3, §3.3.1,
 *               §3.8, §2.2.2, §2.3.0, §7.2, §7.4
 *
 * このファイルの原則（SPEC §2.2.2）:
 *   - クライアントウィンドウ宛のリクエストは常にエラーを許容する。
 *     リプライを必要としない要求は unchecked（xcb_foo）で投げ、エラーは
 *     event_handle_error() が「クライアント宛」として黙って捨てる。
 *   - リプライが必要な場面だけ checked（xcb_foo + xcb_foo_reply(&err)）で発行し、
 *     エラーはこの場で回収して握る。イベントループへ漏らさない。
 *   - 自分で作った frame 宛のエラーは実装バグなので、握り潰す細工はしない。
 */
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/sync.h>

#include "w98wm.h"

/* ================================================================== *
 * メトリクス (SPEC §4.2)
 *
 * Phase 2 で theme.c へ移す。それまでの唯一の定義場所としてここに置く。
 * 数値を他所へ直書きしないこと（移設時に取りこぼす）。
 * ================================================================== */


/* ================================================================== *
 * ICCCM §4.1.3.1 の WM_STATE 値
 * Xlib の Xutil.h にしかないため、ここで定義する（libxcb は持たない）。
 * ================================================================== */

#define WM_STATE_WITHDRAWN 0u
#define WM_STATE_NORMAL    1u
#define WM_STATE_ICONIC    3u

/* WM_HINTS (ICCCM §4.1.2.4) のうちここで使う分 */
#define WM_HINTS_STATE_HINT (1u << 1)   /* flags: initial_state が有効 */
#define WM_HINTS_LEN        9           /* CARD32 9 語 */

/* _MOTIF_WM_HINTS (§3.2) */
#define MWM_HINTS_LEN         5         /* CARD32 5 語 */
#define MWM_HINTS_DECORATIONS (1u << 1) /* flags: decorations が有効 */

/* frame のイベントマスク。
 * SubstructureRedirect/Notify は子（クライアント）の要求を横取りするため、
 * ButtonPress/Release/PointerMotion は装飾のドラッグとボタン操作、
 * Exposure は装飾の再描画（Phase 2）、EnterWindow は sloppy focus (§3.6) に要る。 */
#define FRAME_EVENT_MASK \
	(XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | \
	 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY   | \
	 XCB_EVENT_MASK_BUTTON_PRESS          | \
	 XCB_EVENT_MASK_BUTTON_RELEASE        | \
	 XCB_EVENT_MASK_POINTER_MOTION        | \
	 XCB_EVENT_MASK_EXPOSURE              | \
	 XCB_EVENT_MASK_ENTER_WINDOW          | \
	 XCB_EVENT_MASK_LEAVE_WINDOW)   /* ボタンのホバー解除に要る (§4.4) */

/* クライアントウィンドウのイベントマスク (§3.1.0)。
 * PropertyChange でタイトル・ヒント・状態の変化を、
 * StructureNotify で自発的な Unmap/Destroy を拾う。 */
#define CLIENT_EVENT_MASK \
	(XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY)

/* ================================================================== *
 * 内部ヘルパ
 * ================================================================== */

/*
 * CARD32 配列のプロパティを want 語だけ読む。
 *
 * 型を問わない（XCB_GET_PROPERTY_TYPE_ANY）のは _MOTIF_WM_HINTS のため。
 * 型として _MOTIF_WM_HINTS を書くクライアントと CARDINAL を書くクライアントが
 * 両方存在し、型を厳密に照合すると Motif ヒントを取りこぼす。
 *
 * 戻り値は実際に out へ書いた語数。プロパティが無い・短い・format != 32 なら 0。
 * GetProperty はリプライを伴うので checked 形で出し、BadWindow はここで握る
 * （クライアントはいつでも消え得る。§2.2.2）。
 */
static size_t get_card32_prop(xcb_window_t win, xcb_atom_t prop,
                              uint32_t *out, size_t want)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_generic_error_t *err = NULL;
	size_t got;
	const uint32_t *src;

	cookie = xcb_get_property(wm.conn, 0, win, prop,
	                          XCB_GET_PROPERTY_TYPE_ANY, 0, (uint32_t)want);
	reply = xcb_get_property_reply(wm.conn, cookie, &err);
	if (reply == NULL) {
		free(err);
		return 0;
	}
	if (reply->type == XCB_ATOM_NONE || reply->format != 32) {
		free(reply);
		return 0;
	}
	got = (size_t)xcb_get_property_value_length(reply) / 4u;
	if (got > want)
		got = want;
	src = (const uint32_t *)xcb_get_property_value(reply);
	for (size_t i = 0; i < got; i++)
		out[i] = src[i];
	free(reply);
	return got;
}

/* このクライアントが「装飾なしの CSD」かどうか (§7.2) */
static bool client_is_csd(const struct client *c)
{
	return (c->flags & CF_CSD) != 0;
}

/*
 * _MOTIF_WM_HINTS の decorations が 0 か (§3.2)。
 * flags に MWM_HINTS_DECORATIONS が立っていて、かつ decorations == 0 の時だけ真。
 * ヘッダに専用の icccm_* 読み取りが無いためここで読む（Phase 3 で icccm.c へ移す）。
 */
static bool motif_says_undecorated(xcb_window_t win)
{
	uint32_t hints[MWM_HINTS_LEN];
	size_t n;

	memset(hints, 0, sizeof hints);
	n = get_card32_prop(win, atoms[ATOM_MOTIF_WM_HINTS], hints, MWM_HINTS_LEN);
	if (n < 3)
		return false;
	if ((hints[0] & MWM_HINTS_DECORATIONS) == 0)
		return false;
	return hints[2] == 0;
}

/*
 * WM_HINTS.initial_state == Iconic か (ICCCM §4.1.2.4, §4.1.4)。
 * 「起動直後から最小化されたい」アプリのため。ヘッダの struct client には
 * initial_state を持つ場所が無いので、管理開始時にここで 1 回だけ見る。
 */
static bool wants_initial_iconic(xcb_window_t win)
{
	uint32_t h[WM_HINTS_LEN];
	size_t n;

	memset(h, 0, sizeof h);
	n = get_card32_prop(win, XCB_ATOM_WM_HINTS, h, WM_HINTS_LEN);
	if (n < 2)
		return false;
	if ((h[0] & WM_HINTS_STATE_HINT) == 0)
		return false;
	return h[1] == WM_STATE_ICONIC;
}

/* 種別と状態から基本レイヤを決める (§3.7.1)。
 * transient による引き上げは stack_effective_layer() の担当。 */
static uint8_t base_layer(const struct client *c)
{
	if (c->states & ST_FULLSCREEN)
		return LAYER_FULLSCREEN;   /* フォーカス条件は stack.c が落とす (§3.7.1) */
	if (c->states & ST_BELOW)
		return LAYER_BELOW;
	if (c->states & ST_ABOVE)
		return LAYER_DOCK;
	return type_props(c->type)->layer;
}

/* min == max のウィンドウは CF_FIXED_SIZE。ボーダー幅が 3px になる (§4.2) */
static void update_fixed_size(struct client *c)
{
	const uint32_t need = HINT_MIN_SIZE | HINT_MAX_SIZE;

	c->flags &= ~(uint32_t)CF_FIXED_SIZE;
	if ((c->hints.flags & need) == need &&
	    c->hints.min_w == c->hints.max_w &&
	    c->hints.min_h == c->hints.max_h &&
	    c->hints.min_w != 0 && c->hints.min_h != 0)
		c->flags |= CF_FIXED_SIZE;
}

/* ================================================================== *
 * 装飾の判定 (SPEC §3.2)
 * ================================================================== */

/*
 * 優先順位は SPEC §3.2 の並びそのまま。上ほど強い。
 *   1. override_redirect      → 装飾しない（そもそも管理対象外）
 *   2. _MOTIF_WM_HINTS の decorations == 0 → 装飾しない
 *   3. CSD (_GTK_FRAME_EXTENTS を持つ)     → 装飾しない (§7.2)
 *   4. 全画面中                             → 装飾しない (§3.5)
 *   5. それ以外は種別表 (§5.2.2)
 * 種別による判定が一番弱いことが要点（§5.2.2 決定規則 4）。
 *
 * 1. について: client_manage() が override_redirect のウィンドウを弾くため、
 * 管理下のクライアントに override_redirect は存在しない。ヘッダにも
 * CF_OVERRIDE_REDIRECT は無い。よってここでの再問い合わせは行わない
 * （この関数は PropertyNotify のたびに呼ばれるので、往復を増やさない）。
 */
bool client_decides_decoration(struct client *c)
{
	/* force_ssd は _MOTIF_WM_HINTS と CSD 判定を無視して装飾を強制する
	 * best-effort モード (§7.2)。全画面と種別表には効かせない。 */
	if (!wm.cfg.force_ssd) {
		if (motif_says_undecorated(c->win))
			return false;
		if (client_is_csd(c))
			return false;
	}
	if (c->states & ST_FULLSCREEN)
		return false;
	return type_props(c->type)->decorated;
}

/* ================================================================== *
 * 装飾の厚み (SPEC §4.2)
 * ================================================================== */

void client_frame_offsets(const struct client *c,
                          uint16_t *left, uint16_t *right,
                          uint16_t *top, uint16_t *bottom)
{
	uint16_t border, caption;
	const struct metrics *m;

	/* 装飾なし（undecorated / CSD / 全画面）は全辺 0。
	 * frame は作るがボーダーとキャプションを 0 にする、という §3.2 / §7.2 の規定。 */
	if ((c->flags & CF_DECORATED) == 0) {
		*left = *right = *top = *bottom = 0;
		return;
	}

	/*
	 * メトリクスは theme_metrics() から取る。生の定数をここに持つと
	 * deco.c と二重定義になり、scale の扱いが片方だけ変わった瞬間に
	 * 「描画位置と当たり判定がずれる」形で壊れる。値は既に scale 倍済み。
	 */
	m = theme_metrics();

	border  = (c->flags & CF_FIXED_SIZE) ? m->border_fixed : m->border_sizing;
	caption = type_props(c->type)->small_caption ? m->caption_h_small
	                                             : m->caption_h;

	*left = *right = *bottom = border;
	*top  = (uint16_t)(border + caption);
}

/*
 * 可視矩形 V (SPEC §7.2)
 *
 * SSD なら frame の外形、CSD なら _GTK_FRAME_EXTENTS（影と不可視リサイズ
 * ボーダー）を差し引いた「利用者に見えている矩形」。
 * 配置・スナップ・最大化・モニタ帰属判定はすべてこれを基準にする。
 * 各モジュールが個別に同じ計算を持つと必ず食い違うため、ここに集約する。
 */
void client_visual_rect(const struct client *c, struct rect *out)
{
	uint16_t l, r, t, b;

	if (c->flags & CF_CSD) {
		/* GTK は全画面時に extents を 0 に更新するので、その都度読んだ値を使う */
		int32_t x = c->geom.x + (int32_t)c->gtk_extents[0];
		int32_t y = c->geom.y + (int32_t)c->gtk_extents[2];
		int32_t w = (int32_t)c->geom.w - c->gtk_extents[0] - c->gtk_extents[1];
		int32_t h = (int32_t)c->geom.h - c->gtk_extents[2] - c->gtk_extents[3];

		out->x = (int16_t)x;
		out->y = (int16_t)y;
		out->w = (uint16_t)(w > 0 ? w : 1);
		out->h = (uint16_t)(h > 0 ? h : 1);
		return;
	}

	client_frame_offsets(c, &l, &r, &t, &b);
	out->x = (int16_t)(c->geom.x - l);
	out->y = (int16_t)(c->geom.y - t);
	out->w = (uint16_t)(c->geom.w + l + r);
	out->h = (uint16_t)(c->geom.h + t + b);
}

/* ================================================================== *
 * ジオメトリの反映
 * ================================================================== */

/*
 * frame と win を配置し直す。
 *
 * ここが「クライアント矩形 → frame 矩形」の唯一の変換点である。
 * CSD (§7.2) ではジオメトリの基準がウィンドウ実矩形 W ではなく
 * 可視矩形 V = W - (l,r,t,b) になるが、Phase 1 は装飾を描かないため
 * client_frame_offsets() が全辺 0 を返し、frame == W で一致する。
 * Phase 2 以降で V 基準の補正を入れる場合も、変更するのはこの関数だけで済む。
 * 呼び出し側が自前で geom ± オフセットを計算し始めたら、その時点で破綻する。
 */
void client_apply_geometry(struct client *c)
{
	uint16_t l, r, t, b;
	uint32_t vals[4];
	int32_t fx, fy, fw, fh;

	client_frame_offsets(c, &l, &r, &t, &b);

	fx = (int32_t)c->geom.x - (int32_t)l;
	fy = (int32_t)c->geom.y - (int32_t)t;
	fw = (int32_t)c->geom.w + (int32_t)l + (int32_t)r;
	fh = (int32_t)c->geom.h + (int32_t)t + (int32_t)b;

	/* X のウィンドウは幅・高さ 0 を許さない（BadValue）。飽和させる。 */
	if (fw < 1)     fw = 1;
	if (fh < 1)     fh = 1;
	if (fw > 65535) fw = 65535;
	if (fh > 65535) fh = 65535;

	vals[0] = (uint32_t)(int32_t)(int16_t)fx;
	vals[1] = (uint32_t)(int32_t)(int16_t)fy;
	vals[2] = (uint32_t)fw;
	vals[3] = (uint32_t)fh;
	xcb_configure_window(wm.conn, c->frame,
	    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);

	/* 子は frame 内の相対座標。border_width は 0 のまま (§3.1.0) */
	vals[0] = (uint32_t)l;
	vals[1] = (uint32_t)t;
	vals[2] = c->geom.w != 0 ? c->geom.w : 1u;
	vals[3] = c->geom.h != 0 ? c->geom.h : 1u;
	xcb_configure_window(wm.conn, c->win,
	    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
}

/*
 * synthetic ConfigureNotify (ICCCM §4.1.5)。
 *
 * ConfigureRequest を無視した / 別の値に丸めた時と、frame ごと移動して
 * クライアントのルート相対位置だけが変わった時に送る。後者は実 ConfigureNotify が
 * 発生しない（子の frame 内相対位置は不変）ため、これを送らないと
 * ポップアップの位置合わせをするアプリが古い座標を使い続ける。
 *
 * 座標は必ずルート相対、border_width は 0（実際に 0 にしてある。§3.1.0）。
 */
void client_send_configure(struct client *c)
{
	xcb_configure_notify_event_t ev;

	memset(&ev, 0, sizeof ev);
	ev.response_type     = XCB_CONFIGURE_NOTIFY;
	ev.event             = c->win;
	ev.window            = c->win;
	ev.above_sibling     = XCB_WINDOW_NONE;
	ev.x                 = c->geom.x;
	ev.y                 = c->geom.y;
	ev.width             = c->geom.w;
	ev.height            = c->geom.h;
	ev.border_width      = 0;
	ev.override_redirect = 0;

	xcb_send_event(wm.conn, 0, c->win,
	               XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
}

/* ================================================================== *
 * WM_STATE (ICCCM §4.1.3.1)
 * ================================================================== */

void client_set_state(struct client *c, uint32_t wm_state)
{
	uint32_t data[2];

	data[0] = wm_state;
	data[1] = XCB_WINDOW_NONE;   /* icon window は持たない */
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->win,
	                    atoms[ATOM_WM_STATE], atoms[ATOM_WM_STATE], 32,
	                    2, data);
}

void client_update_frame_extents(struct client *c)
{
	/* 実際の値の組み立ては ewmh.c が client_frame_offsets() から行う。
	 * CSD クライアントには (0,0,0,0) になる（オフセットが全辺 0 のため。§7.2） */
	ewmh_set_frame_extents(c);
}

/* ================================================================== *
 * 検索
 * ================================================================== */

/* スタックの線形走査。数百規模なので十分（§2.3.0 の上限は 512） */
struct client *client_find(xcb_window_t w)
{
	struct client *c;

	if (w == XCB_WINDOW_NONE)
		return NULL;
	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->win == w || c->frame == w)
			return c;
	}
	return NULL;
}

struct client *client_find_by_frame(xcb_window_t f)
{
	struct client *c;

	if (f == XCB_WINDOW_NONE)
		return NULL;
	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->frame == f)
			return c;
	}
	return NULL;
}

/* ================================================================== *
 * 管理の開始 (SPEC §3.1, §3.1.0, §3.1.1)
 * ================================================================== */

struct client *client_manage(xcb_window_t win, bool adopting)
{
	xcb_get_window_attributes_cookie_t acookie;
	xcb_get_window_attributes_reply_t *attr;
	xcb_get_geometry_cookie_t gcookie;
	xcb_get_geometry_reply_t *geo;
	xcb_generic_error_t *err = NULL;
	struct client *c;
	uint32_t vals[3];
	uint32_t wm_state_val = 0;
	uint32_t desktop = 0;
	uint16_t l, r, t, b;
	bool has_wm_state, was_mapped, iconic;
	uint8_t depth;

	if (win == XCB_WINDOW_NONE || win == wm.root || win == wm.check_win ||
	    win == wm.focus_win)
		return NULL;
	if (client_find(win) != NULL)
		return NULL;   /* 既に管理下（MapRequest の重複など） */

	/*
	 * 存在確認は checked (§2.2.2)。
	 * MapRequest を受けてからここへ来るまでにウィンドウが死ぬのは通常の動作条件で、
	 * 異常ではない。リプライが取れなければ黙って諦める。
	 * override_redirect の確認も同じリプライで済ませる（往復を増やさない）。
	 */
	acookie = xcb_get_window_attributes(wm.conn, win);
	attr = xcb_get_window_attributes_reply(wm.conn, acookie, &err);
	if (attr == NULL) {
		free(err);
		return NULL;   /* もう無い。ログも出さない */
	}
	if (attr->override_redirect) {
		free(attr);
		return NULL;   /* 管理対象外 (§3.2, §5.2.2-3) */
	}

	/* WM_STATE の有無と値。adopt の判定と初期状態の両方に使う。 */
	has_wm_state = prop_get_card32(win, atoms[ATOM_WM_STATE],
	                               atoms[ATOM_WM_STATE], &wm_state_val);

	/*
	 * 起動時の取り込み条件 (PLAN Phase 1-2):
	 *   map_state == Viewable、または WM_STATE を持つもの。
	 * 後者を落とすと、前の WM が Iconic にしたウィンドウ
	 * （unmapped + WM_STATE=Iconic）を取りこぼして復帰不能になる。
	 */
	if (adopting && attr->map_state != XCB_MAP_STATE_VIEWABLE && !has_wm_state) {
		free(attr);
		return NULL;
	}

	/* ジオメトリと border_width を 1 往復で取る (§3.1.0) */
	gcookie = xcb_get_geometry(wm.conn, win);
	geo = xcb_get_geometry_reply(wm.conn, gcookie, &err);
	if (geo == NULL) {
		free(err);
		free(attr);
		return NULL;   /* 取得までの間に消えた */
	}

	/*
	 * 確保失敗・上限超過 (§2.3.0)。
	 * クライアントを殺してはならない。フレームを作らず、装飾なしの
	 * 非管理ウィンドウとしてそのまま map する。アプリは使えるままになる。
	 * 以後、枠が空いても遡って管理はしない。
	 */
	c = slab_alloc();
	if (c == NULL) {
		ERR("client slab exhausted (%u managed); mapping %u unmanaged (SPEC 2.3.0)",
		    (unsigned)slab_count(), (unsigned)win);
		xcb_map_window(wm.conn, win);
		free(geo);
		free(attr);
		return NULL;
	}
	memset(c, 0, sizeof *c);

	c->win         = win;
	c->geom.x      = geo->x;
	c->geom.y      = geo->y;
	c->geom.w      = geo->width  != 0 ? geo->width  : 1u;
	c->geom.h      = geo->height != 0 ? geo->height : 1u;
	c->border_orig = geo->border_width;   /* unmanage で復元する (§3.1.0) */
	c->desktop     = wm.current_desktop;
	c->type        = TYPE_NORMAL;
	c->sync_state  = SYNC_NONE;
	c->flags       = adopting ? CF_ADOPTED : 0u;

	was_mapped = (attr->map_state != XCB_MAP_STATE_UNMAPPED);

	free(geo);
	free(attr);

	/*
	 * クライアントの border_width を 0 にする (§3.1.0, ICCCM)。
	 * xterm は既定で border_width = 1 を持っており、0 にしないと装飾の内側に
	 * 意図しない枠が透けて出る。元の値は c->border_orig に退避済み。
	 */
	vals[0] = 0;
	xcb_configure_window(wm.conn, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, vals);

	/* frame の生成 (§3.1)。ルートと同じ depth / visual なので colormap は不要。
	 * backing store は持たない（サーバ側メモリを食うだけで、Expose で描き直す）。 */
	depth = wm.depth != 0 ? wm.depth : (uint8_t)XCB_COPY_FROM_PARENT;
	c->frame = xcb_generate_id(wm.conn);
	vals[0] = wm.screen != NULL ? wm.screen->white_pixel : 0u;  /* Phase 2 で theme.c の face 色に置き換える */
	vals[1] = XCB_BACKING_STORE_NOT_USEFUL;
	vals[2] = FRAME_EVENT_MASK;
	xcb_create_window(wm.conn, depth, c->frame, wm.root,
	                  c->geom.x, c->geom.y, c->geom.w, c->geom.h, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_BACKING_STORE |
	                  XCB_CW_EVENT_MASK, vals);

	/*
	 * ★ セーブセット (§3.1.1) — reparent する「前」に INSERT する。
	 *
	 * これが無いと、w98wm が落ちた時に全ての管理ウィンドウが道連れになる。
	 * 理由: 接続が切れるとサーバは w98wm が作ったリソース（= frame）を破棄し、
	 * DestroyWindow は子孫を再帰的に破棄するため、reparent 済みのアプリの
	 * ウィンドウごと消える。セーブセットに入れたウィンドウだけは、接続断時に
	 * サーバがルートの子へ戻して再マップしてくれる。
	 * 「WM が落ちてもアプリが生き残る」唯一の仕組みであり、正常終了時の
	 * 自前 reparent（client_unmanage）は kill -9 では走らないので代わりにならない。
	 * 通常操作では一切表面化しない。消してはならない。
	 */
	xcb_change_save_set(wm.conn, XCB_SET_MODE_INSERT, c->win);

	/* 装飾の厚みは後で確定するが、reparent の座標が要るので暫定値で置く。
	 * この時点では CF_DECORATED が未設定なので (0,0) になり、
	 * 下の client_apply_geometry() が最終位置へ直す。 */
	client_frame_offsets(c, &l, &r, &t, &b);
	xcb_reparent_window(wm.conn, c->win, c->frame, (int16_t)l, (int16_t)t);

	/*
	 * WM 起因の UnmapNotify を無視するカウンタ (§3.8)。
	 * 発生源は 3 つ: (1) reparent、(2) 最小化、(3) デスクトップ切替。
	 * すべてこの 1 個のカウンタで数える（別々に持つと必ず食い違う）。
	 *
	 * ReparentWindow が UnmapNotify を生むのは「対象がマップ済みの時だけ」
	 * （X プロトコル ReparentWindow の規定）。MapRequest 経路のウィンドウは
	 * まだ unmapped なのでイベントは出ない。ここで無条件に加算すると、
	 * その後クライアントが自発的に withdraw した時の本物の UnmapNotify を
	 * 食い潰し、ウィンドウが画面に残り続ける。
	 */
	if (was_mapped)
		c->unmap_pending++;

	/* クライアントのイベント選択 (§3.1.0)。unmanage で元に戻す。 */
	vals[0] = CLIENT_EVENT_MASK;
	xcb_change_window_attributes(wm.conn, c->win, XCB_CW_EVENT_MASK, vals);

	/* ---- プロパティの読み込み ---- */
	icccm_update_size_hints(c);
	icccm_update_wm_hints(c);
	icccm_update_protocols(c);
	icccm_update_transient(c);
	icccm_update_title(c);
	icccm_update_window_type(c);
	icccm_update_states(c);
	icccm_update_gtk_extents(c);   /* CF_CSD はここで立つ (§7.2) */
	motif_update(c);               /* _MOTIF_WM_HINTS (§3.2) */
	icon_update(c);                /* _NET_WM_ICON (§4.4.1) */
	sync_client_init(c);           /* _NET_WM_SYNC_REQUEST (§7.3) */

	update_fixed_size(c);

	if (client_decides_decoration(c))
		c->flags |= CF_DECORATED;
	else
		c->flags &= ~(uint32_t)CF_DECORATED;

	shape_apply(c);                /* 非矩形ウィンドウの形状追従 (§5.3、任意) */

	/* デスクトップの決定 (§3.8) */
	if (prop_get_card32(c->win, atoms[ATOM_NET_WM_DESKTOP],
	                    XCB_ATOM_CARDINAL, &desktop)) {
		if (desktop == WM_ALL_DESKTOPS || desktop < wm.n_desktops)
			c->desktop = desktop;
	}
	if (c->states & ST_STICKY)
		c->desktop = WM_ALL_DESKTOPS;
	/* 自分で設定していないクライアントのために WM 側から公開しておく。
	 * client_set_desktop() は frame の map/unmap まで行うため、まだ map して
	 * いないこの時点では使わず、プロパティだけ書く。 */
	vals[0] = c->desktop;
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->win,
	                    atoms[ATOM_NET_WM_DESKTOP], XCB_ATOM_CARDINAL, 32,
	                    1, vals);

	c->layer = base_layer(c);

	/*
	 * 初期配置 (§3.3.1)。
	 * adopt したウィンドウは既に画面上の位置がユーザにとって意味を持つので動かさない。
	 */
	if (!adopting)
		layout_place_new(c);

	/*
	 * restore はマップ時点の初期ジオメトリで初期化する (§3.5.1)。
	 * 初期状態で最大化を要求するアプリのために、最大化の適用より先に行う。
	 */
	c->restore = c->geom;
	c->pre_fs  = c->geom;

	client_apply_geometry(c);

	stack_add(c);
	focus_mru_promote(c);   /* MRU に載せる。実際のフォーカス付与は focus.c の判断 */
	wm.n_clients++;

	/* ---- 初期状態 (ICCCM §4.1.4) ---- */
	iconic = (adopting && has_wm_state && wm_state_val == WM_STATE_ICONIC) ||
	         (!adopting && wants_initial_iconic(c->win));

	client_update_frame_extents(c);
	ewmh_set_allowed_actions(c);

	if (iconic) {
		c->flags  |= CF_ICONIC;
		c->states |= ST_HIDDEN;
		client_set_state(c, WM_STATE_ICONIC);
		/* frame も win もマップしない。既に unmapped なので unmap も不要 */
	} else if (client_visible_on(c, wm.current_desktop)) {
		xcb_map_window(wm.conn, c->win);
		xcb_map_window(wm.conn, c->frame);
		c->flags |= CF_MAPPED;
		client_set_state(c, WM_STATE_NORMAL);
	} else {
		/* 別デスクトップ: WM_STATE は Normal のまま、HIDDEN も立てない (§3.8) */
		client_set_state(c, WM_STATE_NORMAL);
	}

	ewmh_set_wm_state(c);
	stack_update_client_list();

	/* 配置で frame ごと動いた場合、子のルート相対位置は変わるが実 ConfigureNotify は
	 * 出ない。ICCCM §4.1.5 に従い synthetic を 1 回送る。 */
	client_send_configure(c);

	LOG("manage win=%u frame=%u %dx%d+%d+%d type=%d deco=%d%s",
	    (unsigned)c->win, (unsigned)c->frame,
	    (int)c->geom.w, (int)c->geom.h, (int)c->geom.x, (int)c->geom.y,
	    (int)c->type, (c->flags & CF_DECORATED) ? 1 : 0,
	    adopting ? " adopted" : "");

	return c;
}

/* ================================================================== *
 * 管理の解除 (SPEC §3.1.1, §2.2.2, §7.3, §4.4.1)
 * ================================================================== */

void client_unmanage(struct client *c, bool destroyed)
{
	uint32_t vals[1];

	if (c == NULL)
		return;

	/*
	 * 二重 unmanage の防止 (§2.2.2)。
	 * UnmapNotify で withdraw 処理に入った直後に DestroyNotify が来る競合は
	 * 実際に起きる。CF_DESTROYED を「解体に入った」印として使い、
	 * 2 回目以降は何もしない。これが無いと解放済み構造体を触る。
	 */
	if (c->flags & CF_DESTROYED)
		return;
	c->flags |= CF_DESTROYED;

	/*
	 * ドラッグ中ならセッションを破棄する。
	 * これを忘れると slab_free 後の領域をドラッグ処理が参照し続ける
	 * （解放済みメモリが読めてしまうぶん、症状が出にくい不具合になる）。
	 */
	move_forget(c);

	if (!destroyed) {
		/*
		 * ウィンドウはまだ生きている。ICCCM の Withdrawn 遷移として後始末する。
		 * この経路のリクエストは全てエラー許容（unchecked）。
		 * ここに来た時点で既に破棄されていることがあり、その場合は
		 * BadWindow がイベントとして届いて捨てられるだけになる (§2.2.2)。
		 */

		/* イベント選択を解除する (§3.1.0)。
		 * 先に外さないと、以下の reparent が自分宛の UnmapNotify を生む。 */
		vals[0] = XCB_EVENT_MASK_NO_EVENT;
		xcb_change_window_attributes(wm.conn, c->win, XCB_CW_EVENT_MASK, vals);

		/* border_width を元に戻す (§3.1.0) */
		vals[0] = c->border_orig;
		xcb_configure_window(wm.conn, c->win,
		                     XCB_CONFIG_WINDOW_BORDER_WIDTH, vals);

		/* ルートへ戻す。座標はクライアント矩形のルート相対位置。 */
		xcb_reparent_window(wm.conn, c->win, wm.root, c->geom.x, c->geom.y);

		/*
		 * 最小化中（＝WM の都合で unmap してある）ウィンドウは、
		 * ルートへ戻しただけでは二度と見えない。WM 終了時に
		 * ユーザのウィンドウを消してしまわないよう map し直す。
		 * クライアント自身が unmap した withdraw の場合は CF_ICONIC が
		 * 立っていないので、ここは通らない（勝手に map し返さない）。
		 */
		if (c->flags & CF_ICONIC)
			xcb_map_window(wm.conn, c->win);

		/* セーブセットから外す (§3.1.1)。もう frame の子ではない。 */
		xcb_change_save_set(wm.conn, XCB_SET_MODE_DELETE, c->win);

		/* WM_STATE を消して Withdrawn にする (ICCCM §4.1.3.1) */
		xcb_delete_property(wm.conn, c->win, atoms[ATOM_WM_STATE]);
	}
	/* destroyed の場合、死んだウィンドウ宛のリクエストは全て BadWindow を
	 * 生むだけなので 1 つも出さない。セーブセットの登録もサーバ側で消えている。 */

	/*
	 * サーバ側リソースの解放。frame は自分のリソースなのでエラーは出ない。
	 */
	if (c->frame != XCB_WINDOW_NONE)
		xcb_destroy_window(wm.conn, c->frame);

	/*
	 * 同期アラームの破棄 (§7.3)。
	 * 忘れるとサーバ側リソースがウィンドウ数に比例して残り、§9.1 の指標 C
	 * （サーバ側リソースが線形）を静かに破る。通常操作では表面化しない。
	 * カウンタ側が先に消えていると BadAlarm になるが、§2.2.2 に従い無視でよい。
	 */
	sync_client_fini(c);

	/*
	 * アイコンの解放 (§4.4.1)。所有者の区別は icon.c が持つ——
	 * WM が作った Pixmap は解放し、WM_HINTS 由来のクライアント所有 ID は
	 * 絶対に解放しない（他プロセスの資源を壊すため）。
	 */
	icon_free(c);

	/* リストから外してからフォーカスを渡す。
	 * 先に外すのは、focus_next_after() が候補走査でこの client を拾わないため。 */
	focus_mru_remove(c);
	stack_remove(c);
	if (wm.n_clients > 0)
		wm.n_clients--;

	if (wm.focused == c)
		wm.focused = NULL;
	focus_next_after(c);

	stack_update_client_list();

	LOG("unmanage win=%u frame=%u%s", (unsigned)c->win, (unsigned)c->frame,
	    destroyed ? " (destroyed)" : "");

	slab_free(c);
}

/* ================================================================== *
 * 最小化・復帰 (SPEC §3.3, §3.8)
 * ================================================================== */

void client_iconify(struct client *c)
{
	struct client *o;

	if (c == NULL || (c->flags & CF_ICONIC) != 0)
		return;

	/* 先にフラグを立てる。下の transient 走査がこの client へ戻ってきても
	 * 再帰しないようにするため (§3.7.2 の循環対策と同じ理由)。 */
	c->flags |= CF_ICONIC;

	/*
	 * WM 起因の UnmapNotify を無視するカウンタ (§3.8 の発生源 2)。
	 * 加算は必ず UnmapWindow の「前」。後にすると、サーバからのイベントが
	 * 先に読まれた場合に本物の withdraw と区別できなくなる。
	 *
	 * ★ カウンタの単位は「クライアントウィンドウ (c->win) 宛の UnmapNotify」
	 * ちょうど 1 件である。減算するのは event.c の on_unmap_notify() だけで、
	 * そこは e->window != c->win のイベントを数える前に捨てている。
	 * したがって frame だけを unmap する経路で加算してはならない
	 * （frame の UnmapNotify は永久に減算されず、次の本物の withdraw を
	 * 食い潰してウィンドウが画面に残る）。逆に c->win を unmap する経路では
	 * 必ず加算する。ここで両方 unmap しているので加算は 1 回だけ。
	 */
	c->unmap_pending++;
	xcb_unmap_window(wm.conn, c->win);
	xcb_unmap_window(wm.conn, c->frame);

	c->flags  &= ~(uint32_t)CF_MAPPED;
	c->states |= ST_HIDDEN;
	client_set_state(c, WM_STATE_ICONIC);
	ewmh_set_wm_state(c);

	/* 親が最小化されたら transient も一緒に最小化する (§3.7.2) */
	for (o = wm.stack_bottom; o != NULL; o = o->next) {
		if (o != c && (o->flags & CF_ICONIC) == 0 &&
		    stack_is_transient_of(o, c))
			client_iconify(o);
	}

	if (wm.focused == c) {
		wm.focused = NULL;
		focus_next_after(c);
	}
}

void client_deiconify(struct client *c)
{
	if (c == NULL || (c->flags & CF_ICONIC) == 0)
		return;

	c->flags &= ~(uint32_t)CF_ICONIC;

	/* 別デスクトップにあるウィンドウを復帰させても、そこは表示面ではない (§3.8)。
	 * WM_STATE だけ Normal に戻し、map は切替時に行う。 */
	if (client_visible_on(c, wm.current_desktop)) {
		xcb_map_window(wm.conn, c->win);
		xcb_map_window(wm.conn, c->frame);
		c->flags |= CF_MAPPED;
	}

	c->states &= ~(uint32_t)ST_HIDDEN;
	client_set_state(c, WM_STATE_NORMAL);
	ewmh_set_wm_state(c);
}

/* ================================================================== *
 * 閉じる (SPEC §7.4)
 * ================================================================== */

void client_close(struct client *c)
{
	xcb_client_message_event_t ev;

	if (c == NULL)
		return;

	if (c->flags & CF_DELETE_WINDOW) {
		/* ICCCM §4.2.8: WM_PROTOCOLS / WM_DELETE_WINDOW をクライアントへ送る。
		 * タイムスタンプは本来「操作の時刻」だが、ヘッダに最終イベント時刻の
		 * 保管場所が無いため CurrentTime を使う。 */
		memset(&ev, 0, sizeof ev);
		ev.response_type  = XCB_CLIENT_MESSAGE;
		ev.format         = 32;
		ev.window         = c->win;
		ev.type           = atoms[ATOM_WM_PROTOCOLS];
		ev.data.data32[0] = atoms[ATOM_WM_DELETE_WINDOW];
		ev.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(wm.conn, 0, c->win, XCB_EVENT_MASK_NO_EVENT,
		               (const char *)&ev);
		return;
	}

	/*
	 * WM_DELETE_WINDOW を持たないクライアントは KillClient で落とす (§7.4)。
	 * X 接続ごと切るので PID は不要で、リモート表示のクライアントにも効く。
	 * Win98 風の「プログラムの終了」確認ダイアログは Phase 4。
	 * それまではログに残してから落とす。
	 */
	LOG("client %u has no WM_DELETE_WINDOW; falling back to KillClient (SPEC 7.4)",
	    (unsigned)c->win);
	xcb_kill_client(wm.conn, c->win);
}
