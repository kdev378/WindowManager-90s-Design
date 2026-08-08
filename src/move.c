/*
 * move.c - 対話的な移動・リサイズ (SPEC §3.4, §3.4.1)
 *
 * このファイルの心臓部は §3.4.1 の「ドラッグ中のフレームペーシング」である。
 * 4 つの規則（合体・レート上限・未確定 1 件・終端の即時確定）は、どれか 1 つでも
 * 落とすと低速なリモート X や重量級クライアントでドラッグが破綻する。
 * 各規則の実装箇所には「これが無いと何が壊れるか」を 1 行で添えてある。
 * 「素朴に書き直す」ことは仕様違反である。
 *
 * ジオメトリの基準:
 *   struct client.geom はクライアント領域のルート座標である。
 *   ユーザから見える矩形（visual rect, §7.2 の V）は
 *     V = geom を装飾ぶん外へ広げ、CSD の不可視余白ぶん内へ縮めたもの
 *   であり、スナップ判定はすべて V に対して行う。装飾の厚みぶんずれた位置に
 *   吸着すると、見た目が画面端に揃わない。
 */

#include <limits.h>
#include <stdlib.h>

#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * ドラッグセッション
 *   モーション経路で確保を行わないため、セッションは単一の static に持つ。
 *   同時に走るドラッグは 1 つだけ（ポインタは 1 本しかない）。
 * ------------------------------------------------------------------ */

struct drag_session {
	struct client  *c;
	enum drag_kind  kind;
	uint8_t         edge;        /* EDGE_* のビット和。DRAG_MOVE では 0 */

	int16_t         ptr_x, ptr_y;   /* grab 時のポインタ（ルート座標） */
	struct rect     start;          /* 追従の基準になるジオメトリ */

	/* 取り消し用（§7.2 の _NET_WM_MOVERESIZE CANCEL / Esc） */
	struct rect     orig;           /* move_begin 時点の geom */
	struct rect     orig_restore;   /* move_begin 時点の restore (§3.5.1) */
	uint32_t        orig_states;
	bool            unmaximized;    /* 開始時に最大化を解除したか */

	/* §3.4.1-2/3 の保留ジオメトリ。まだ送っていない最新の希望値 */
	struct rect     pending;
	bool            has_pending;
};

static struct drag_session drag;

/* ------------------------------------------------------------------ *
 * 小物
 * ------------------------------------------------------------------ */

struct box { int l, t, r, b; };   /* 端の座標。int で持ち桁あふれを避ける */

struct snap {
	int delta;   /* 適用すべきずらし量 */
	int dist;    /* その候補の距離。より近い候補で上書きする */
};

static int16_t  clamp_i16(int v);
static uint16_t clamp_u16(int v);
static void     drag_insets(const struct client *c,
                            int *l, int *r, int *t, int *b);
static void     visual_box(const struct client *c, struct box *out);
static bool     snap_candidate(const struct client *o);
static void     snap_try(struct snap *s, int pos, int target, int limit);
static void     snap_collect(struct snap *slo, struct snap *shi,
                             int lo, int hi, int plo, int phi,
                             bool horiz, int limit);
static void     drag_compute(struct rect *out, int16_t rx, int16_t ry);
static void     drag_stash(const struct rect *g);
static void     drag_apply(const struct rect *g, uint64_t now);
static void     drag_sync_request(struct client *c, uint64_t now);
static bool     drag_client_gone(void);
static void     drag_coalesce(int16_t *rx, int16_t *ry,
                              xcb_generic_event_t **deferred);
static void     drag_update_restore(struct client *c);

static int16_t clamp_i16(int v)
{
	if (v < INT16_MIN) return INT16_MIN;
	if (v > INT16_MAX) return INT16_MAX;
	return (int16_t)v;
}

static uint16_t clamp_u16(int v)
{
	if (v < 1) return 1;              /* 幅・高さ 0 は BadValue になる */
	if (v > 0xFFFF) return 0xFFFF;
	return (uint16_t)v;
}

/*
 * geom から visual rect への余白。
 * SSD なら装飾の厚み、CSD なら _GTK_FRAME_EXTENTS ぶんの負の余白 (§7.2)。
 * 両方が同時に効くことは無いが、式は 1 本にまとめておく。
 */
static void drag_insets(const struct client *c, int *l, int *r, int *t, int *b)
{
	uint16_t fl = 0, fr = 0, ft = 0, fb = 0;

	client_frame_offsets(c, &fl, &fr, &ft, &fb);
	*l = (int)fl;
	*r = (int)fr;
	*t = (int)ft;
	*b = (int)fb;

	if (c->flags & CF_CSD) {
		*l -= (int)c->gtk_extents[0];
		*r -= (int)c->gtk_extents[1];
		*t -= (int)c->gtk_extents[2];
		*b -= (int)c->gtk_extents[3];
	}
}

static void visual_box(const struct client *c, struct box *out)
{
	int il, ir, it, ib;

	drag_insets(c, &il, &ir, &it, &ib);
	out->l = (int)c->geom.x - il;
	out->t = (int)c->geom.y - it;
	out->r = (int)c->geom.x + (int)c->geom.w + ir;
	out->b = (int)c->geom.y + (int)c->geom.h + ib;
}

/* スナップの相手にしてよいクライアントか */
static bool snap_candidate(const struct client *o)
{
	if (o == drag.c)                       return false;
	if (o->flags & CF_DESTROYED)           return false;
	if (!(o->flags & CF_MAPPED))           return false;
	if (o->flags & CF_ICONIC)              return false;
	if (o->states & (ST_HIDDEN | ST_FULLSCREEN)) return false;
	/* デスクトップ型は画面全体を覆う。作業領域の候補と重複するだけなので除く */
	if (o->type == TYPE_DESKTOP)           return false;
	if (o->desktop != WM_ALL_DESKTOPS && o->desktop != wm.current_desktop)
		return false;
	return true;
}

static void snap_try(struct snap *s, int pos, int target, int limit)
{
	int d  = target - pos;
	int ad = (d < 0) ? -d : d;

	if (ad <= limit && ad < s->dist) {
		s->dist  = ad;
		s->delta = d;
	}
}

/*
 * 1 軸ぶんのスナップ候補を集める (SPEC §3.4「画面端・他ウィンドウ端への吸着」)。
 *   slo/shi : lo 側 / hi 側の辺に対する結果。NULL ならその辺は動かさない
 *   lo/hi   : 動かす矩形の当該軸の辺（visual 座標）
 *   plo/phi : 直交軸の範囲。相手と重なっていない候補は無視する
 *   horiz   : true なら x 軸
 * 移動では slo と shi に同じ struct を渡す（矩形全体を 1 つの delta で動かす）。
 */
static void snap_collect(struct snap *slo, struct snap *shi,
                         int lo, int hi, int plo, int phi,
                         bool horiz, int limit)
{
	const struct client *o;
	uint8_t i;

	/* モニタ別作業領域 (§3.5.2)。_NET_WORKAREA ではなくこちらを使う */
	for (i = 0; i < wm.n_monitors; i++) {
		const struct rect *wa = &wm.monitors[i].workarea;
		int tlo, thi, qlo, qhi;

		if (!rect_valid(wa))
			continue;
		tlo = horiz ? (int)wa->x : (int)wa->y;
		thi = tlo + (horiz ? (int)wa->w : (int)wa->h);
		qlo = horiz ? (int)wa->y : (int)wa->x;
		qhi = qlo + (horiz ? (int)wa->h : (int)wa->w);
		if (!(plo < qhi && qlo < phi))
			continue;
		/* 作業領域は内側の辺にのみ吸着させる（外向きの吸着に意味は無い） */
		if (slo) snap_try(slo, lo, tlo, limit);
		if (shi) snap_try(shi, hi, thi, limit);
	}

	/* 他クライアントの visual rect の辺 */
	for (o = wm.stack_bottom; o != NULL; o = o->next) {
		struct box ob;
		int tlo, thi, qlo, qhi;

		if (!snap_candidate(o))
			continue;
		visual_box(o, &ob);
		tlo = horiz ? ob.l : ob.t;
		thi = horiz ? ob.r : ob.b;
		qlo = horiz ? ob.t : ob.l;
		qhi = horiz ? ob.b : ob.r;
		if (!(plo < qhi && qlo < phi))
			continue;
		/* 突き合わせ（辺どうしを接する）と面揃え（同じ側を揃える）の両方 */
		if (slo) {
			snap_try(slo, lo, thi, limit);
			snap_try(slo, lo, tlo, limit);
		}
		if (shi) {
			snap_try(shi, hi, tlo, limit);
			snap_try(shi, hi, thi, limit);
		}
	}
}

/* ------------------------------------------------------------------ *
 * ジオメトリ計算
 * ------------------------------------------------------------------ */

/*
 * ポインタ位置から希望ジオメトリを求める。
 * リサイズでは、掴んだ辺だけを動かし、サイズヒントを適用してから
 * 左/上へ伸びる辺の原点を計算し直す（順序が逆だと、増分を持つ端末の
 * 左辺ドラッグで原点がガタつく。§3.4「文字単位リサイズが正しく効くこと」）。
 */
static void drag_compute(struct rect *out, int16_t rx, int16_t ry)
{
	struct client *c = drag.c;
	int dx = (int)rx - (int)drag.ptr_x;
	int dy = (int)ry - (int)drag.ptr_y;
	int il, ir, it, ib;
	int limit = (int)wm.cfg.snap_distance;
	bool do_snap;
	struct box v;

	drag_insets(c, &il, &ir, &it, &ib);

	/*
	 * 増分を持つクライアントではスナップを行わない (§3.4)。
	 * 吸着で決めた辺の位置を hints_apply が丸め直すため、両者は必ず食い違う。
	 */
	do_snap = (limit > 0) && !(c->hints.flags & HINT_RESIZE_INC);

	if (drag.kind == DRAG_MOVE) {
		struct snap sx = { 0, INT_MAX };
		struct snap sy = { 0, INT_MAX };

		v.l = (int)drag.start.x + dx - il;
		v.t = (int)drag.start.y + dy - it;
		v.r = v.l + (int)drag.start.w + il + ir;
		v.b = v.t + (int)drag.start.h + it + ib;

		if (do_snap) {
			snap_collect(&sx, &sx, v.l, v.r, v.t, v.b, true,  limit);
			snap_collect(&sy, &sy, v.t, v.b, v.l, v.r, false, limit);
		}

		out->x = clamp_i16(v.l + sx.delta + il);
		out->y = clamp_i16(v.t + sy.delta + it);
		out->w = drag.start.w;
		out->h = drag.start.h;
		return;
	}

	/* --- リサイズ --- */
	{
		struct snap sl = { 0, INT_MAX };
		struct snap sr = { 0, INT_MAX };
		struct snap st = { 0, INT_MAX };
		struct snap sb = { 0, INT_MAX };
		int cl, ct, cr, cb;
		uint16_t w, h;

		v.l = (int)drag.start.x - il + ((drag.edge & EDGE_L) ? dx : 0);
		v.t = (int)drag.start.y - it + ((drag.edge & EDGE_T) ? dy : 0);
		v.r = (int)drag.start.x + (int)drag.start.w + ir +
		      ((drag.edge & EDGE_R) ? dx : 0);
		v.b = (int)drag.start.y + (int)drag.start.h + ib +
		      ((drag.edge & EDGE_B) ? dy : 0);

		if (do_snap) {
			snap_collect((drag.edge & EDGE_L) ? &sl : NULL,
			             (drag.edge & EDGE_R) ? &sr : NULL,
			             v.l, v.r, v.t, v.b, true, limit);
			snap_collect((drag.edge & EDGE_T) ? &st : NULL,
			             (drag.edge & EDGE_B) ? &sb : NULL,
			             v.t, v.b, v.l, v.r, false, limit);
			v.l += sl.delta;
			v.r += sr.delta;
			v.t += st.delta;
			v.b += sb.delta;
		}

		/* visual → クライアント領域へ戻す */
		cl = v.l + il;
		ct = v.t + it;
		cr = v.r - ir;
		cb = v.b - ib;

		w = clamp_u16(cr - cl);
		h = clamp_u16(cb - ct);

		/* 増分・最小最大・アスペクトを先に適用する (§3.4) */
		hints_apply(&c->hints, &w, &h);

		/* そのうえで左/上辺の原点を求め直す。逆順にすると左辺がガタつく */
		out->x = clamp_i16((drag.edge & EDGE_L) ? cr - (int)w : cl);
		out->y = clamp_i16((drag.edge & EDGE_T) ? cb - (int)h : ct);
		out->w = w;
		out->h = h;
	}
}

/* ------------------------------------------------------------------ *
 * 送出
 * ------------------------------------------------------------------ */

static void drag_stash(const struct rect *g)
{
	drag.pending     = *g;
	drag.has_pending = true;
}

/*
 * §7.3 の同期リクエスト発行点（リサイズのみ。移動には適用しない）。
 *
 * Phase 1 ではカウンタを持たない扱い（sync_state == SYNC_NONE）なので
 * この関数は何もしない。Phase 3 で以下を埋める:
 *   c->sync_target += 1;
 *   xcb_sync_change_alarm(... trigger = c->sync_target ...);
 *   _NET_WM_SYNC_REQUEST(c->sync_target) を ClientMessage で送る;
 *   c->sync_sent_ms = now; c->sync_state = SYNC_WAITING;
 * WAITING の判定は呼び出し側（move_motion / move_tick）にあるので、
 * この関数はドラッグ中でも終端でも同じように呼んでよい
 * （終端では §7.3「ボタン解放時の扱い」に従い target を採番し直して即送る）。
 */
static void drag_sync_request(struct client *c, uint64_t now)
{
	if (!wm.have_sync)             return;
	if (c->sync_counter == XCB_NONE) return;
	if (c->sync_state == SYNC_NONE)  return;   /* カウンタを持たない */
	/*
	 * §7.3: 一度タイムアウトしたクライアントはセッション中「同期不適合」。
	 * これが無いと、カウンタを持つが更新しないアプリでドラッグの出だしに
	 * 毎回 250 ms の待ちが入る。
	 */
	if (c->flags & CF_SYNC_UNFIT)  return;

	(void)now;   /* Phase 3 で c->sync_sent_ms に入れる */
}

/* 実際にジオメトリを反映する。ここを通ったら last_drag_ms を更新する */
static void drag_apply(const struct rect *g, uint64_t now)
{
	struct client *c = drag.c;

	c->geom          = *g;
	drag.pending     = *g;
	drag.has_pending = false;
	wm.last_drag_ms  = now;

	if (drag.kind == DRAG_RESIZE)
		drag_sync_request(c, now);

	client_apply_geometry(c);
	xcb_flush(wm.conn);
}

/*
 * ドラッグ中にクライアントが消えた場合の保険。
 * ヘッダに client_unmanage → move.c への通知経路が無いため（報告事項）、
 * 各入口で最低限の生存確認を行う。slab は解放後も読み出し自体は安全。
 */
static bool drag_client_gone(void)
{
	if (drag.kind == DRAG_NONE)
		return true;
	if (drag.c == NULL || drag.c->frame == XCB_NONE ||
	    (drag.c->flags & CF_DESTROYED)) {
		drag.kind        = DRAG_NONE;
		drag.c           = NULL;
		drag.has_pending = false;
		xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
		xcb_flush(wm.conn);
		return true;
	}
	return false;
}

/*
 * §3.4.1-1 モーションの合体。
 * これが無いと、1 件の MotionNotify ごとに ConfigureWindow を送ることになり、
 * 低速なリモート X でイベントとリクエストが際限なく滞留してドラッグが破綻する。
 *
 * 構造: イベントループから渡された 1 件を起点に、既にキューへ読み込まれた
 * イベントを xcb_poll_for_queued_event でドレインし、MotionNotify は最新の
 * 1 件だけを残して捨てる。モーション以外に当たった時点でドレインを止め、
 * そのイベントは *deferred として呼び出し元に返す（xcb には押し戻しが無く、
 * 捨てるとイベントを失うため）。呼び出し元は最新モーションを処理し終えてから
 * deferred を event_dispatch へ回す。この順序により
 * 「[motion, motion, ButtonRelease] が並んだとき最後のモーションを取りこぼす」
 * ことが無い。保持するイベントは常に高々 1 件なので確保も配列も要らない。
 */
static void drag_coalesce(int16_t *rx, int16_t *ry,
                          xcb_generic_event_t **deferred)
{
	xcb_generic_event_t *ev;

	*deferred = NULL;
	while ((ev = xcb_poll_for_queued_event(wm.conn)) != NULL) {
		if ((ev->response_type & 0x7F) == XCB_MOTION_NOTIFY) {
			const xcb_motion_notify_event_t *m =
				(const xcb_motion_notify_event_t *)ev;
			*rx = m->root_x;
			*ry = m->root_y;
			free(ev);
			continue;
		}
		*deferred = ev;
		break;
	}
}

/* §3.5.1: restore は「非最大化かつ非全画面」のジオメトリだけを持つ */
static void drag_update_restore(struct client *c)
{
	if (c->states & (ST_MAXIMIZED | ST_FULLSCREEN))
		return;
	if (c->states & ST_SHADED) {
		/* シェード中は高さが見かけ上つぶれている。復元用の高さは保つ */
		c->restore.x = c->geom.x;
		c->restore.y = c->geom.y;
		c->restore.w = c->geom.w;
		return;
	}
	c->restore = c->geom;
}

/* ------------------------------------------------------------------ *
 * 公開 API
 * ------------------------------------------------------------------ */

bool move_active(void)
{
	return drag.kind != DRAG_NONE;
}

void move_begin(struct client *c, enum drag_kind kind, uint8_t edge,
                int16_t root_x, int16_t root_y, xcb_timestamp_t time)
{
	xcb_grab_pointer_cookie_t ck;
	xcb_grab_pointer_reply_t *rep;
	uint16_t mask = XCB_EVENT_MASK_BUTTON_RELEASE |
	                XCB_EVENT_MASK_POINTER_MOTION;
	bool ok;

	if (c == NULL || kind == DRAG_NONE)
		return;
	if (drag.kind != DRAG_NONE)          /* 二重開始は無視 */
		return;
	/* 全画面はモニタ全域に固定。移動もリサイズも受け付けない (§3.5) */
	if (c->states & ST_FULLSCREEN)
		return;
	if (kind == DRAG_RESIZE && (c->flags & CF_FIXED_SIZE))
		return;
	if (kind == DRAG_RESIZE && edge == 0)
		return;

	/*
	 * grab は 1 ジェスチャに 1 回だけなので、ここでの往復は許容する
	 * （§3.4.1 補足が禁じているのは描画経路に往復を置くこと）。
	 * カーソルは Phase 2 で §4.7 の 8 方向ビットマップに差し替える。
	 */
	ck  = xcb_grab_pointer(wm.conn, 0 /* owner_events */, wm.root, mask,
	                       XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
	                       XCB_NONE /* confine_to */, XCB_NONE /* cursor */,
	                       time);
	rep = xcb_grab_pointer_reply(wm.conn, ck, NULL);
	ok  = (rep != NULL && rep->status == XCB_GRAB_STATUS_SUCCESS);
	free(rep);
	if (!ok) {
		ERR("move: grab_pointer failed");
		return;
	}

	drag.c            = c;
	drag.kind         = kind;
	drag.edge         = (kind == DRAG_RESIZE) ? edge : 0;
	drag.ptr_x        = root_x;
	drag.ptr_y        = root_y;
	drag.orig         = c->geom;
	drag.orig_restore = c->restore;
	drag.orig_states  = c->states;
	drag.unmaximized  = false;
	drag.has_pending  = false;

	/*
	 * 最大化中に移動を始めたら、まず最大化を解除してポインタの下へ
	 * 付け直す（Windows と同じ挙動）。
	 * 水平方向はポインタの相対位置を保ち、垂直方向は絶対量を保つ
	 * ——キャプションの高さはウィンドウ幅に比例しないため、
	 * 縦も比例させるとタイトルバーがポインタから離れる。
	 */
	if (kind == DRAG_MOVE && (c->states & ST_MAXIMIZED)) {
		struct rect old = c->geom;
		bool mv = (c->states & ST_MAXIMIZED_VERT) != 0;
		bool mh = (c->states & ST_MAXIMIZED_HORZ) != 0;
		int rel = (old.w > 0)
		        ? ((int)root_x - (int)old.x) * 1000 / (int)old.w
		        : 500;

		layout_maximize(c, mv, mh, false);
		drag.unmaximized = true;

		c->geom.x = clamp_i16((int)root_x - (int)c->geom.w * rel / 1000);
		c->geom.y = old.y;
		client_apply_geometry(c);
	}

	drag.start      = c->geom;
	drag.pending    = c->geom;
	wm.last_drag_ms = wm_now_ms();
	xcb_flush(wm.conn);
}

void move_motion(int16_t root_x, int16_t root_y)
{
	xcb_generic_event_t *deferred = NULL;
	struct client *c;
	struct rect g;
	uint64_t now;

	if (drag_client_gone())
		return;

	/* §3.4.1-1 合体（詳細は drag_coalesce のコメント） */
	drag_coalesce(&root_x, &root_y, &deferred);

	c = drag.c;
	drag_compute(&g, root_x, root_y);
	now = wm_now_ms();

	/*
	 * §3.4.1-3 未確定リクエストは 1 つまで（ドラッグ中のみ）。
	 * これが無いと、同期カウンタの返らないクライアントへ次々に
	 * ConfigureWindow を積み、リサイズ中の描画が破綻してちらつく (§7.3)。
	 * Phase 1 では sync_state が WAITING にならないため素通りする。
	 */
	if (c->sync_state == SYNC_WAITING) {
		drag_stash(&g);
	/*
	 * §3.4.1-2 レート上限（16 ms ≒ 60 Hz）。
	 * これが無いと、ポインタの分解能ぶんだけリクエストを送ることになり、
	 * 送信バイト量とクライアントの再描画コストで低速回線が飽和する。
	 * 早すぎる場合は保留に回し、move_next_timeout_ms が poll(2) を
	 * 残り時間で起こして move_tick が吐き出す（タイマ fd もスレッドも使わない）。
	 */
	} else if (now - wm.last_drag_ms < (uint64_t)WM_DRAG_INTERVAL_MS) {
		drag_stash(&g);
	} else {
		drag_apply(&g, now);
	}

	/* 合体で先読みしてしまった非モーションイベントを本来の順序で流す */
	if (deferred != NULL) {
		event_dispatch(deferred);
		free(deferred);
	}
}

void move_tick(uint64_t now_ms)
{
	struct client *c;

	if (drag_client_gone())
		return;
	c = drag.c;

	/*
	 * §7.3: WAITING のまま 250 ms 過ぎたら STALLED へ落とし、
	 * CF_SYNC_UNFIT を立てて以後のドラッグでは同期を使わない。
	 * 応答しないクライアントがリサイズ操作全体を固まらせてはならない。
	 */
	if (c->sync_state == SYNC_WAITING &&
	    now_ms - c->sync_sent_ms >= (uint64_t)WM_SYNC_TIMEOUT_MS) {
		c->sync_state = SYNC_STALLED;
		c->flags |= CF_SYNC_UNFIT;
		LOG("move: sync timeout, client 0x%08x marked unfit",
		    (unsigned)c->win);
	}

	if (!drag.has_pending)
		return;
	if (c->sync_state == SYNC_WAITING)   /* まだ 1 件が未確定 (§3.4.1-3) */
		return;
	if (now_ms - wm.last_drag_ms < (uint64_t)WM_DRAG_INTERVAL_MS)
		return;

	drag_apply(&drag.pending, now_ms);
}

int move_next_timeout_ms(uint64_t now_ms)
{
	int best = -1;

	if (drag.kind == DRAG_NONE || drag.c == NULL)
		return -1;

	/*
	 * 保留があり、かつ WAITING で塞がれていないときだけレート上限の
	 * 締め切りを申告する。WAITING 中に申告すると、move_tick が何も
	 * 吐き出せないまま poll が起き続ける（＝スピン）。
	 */
	if (drag.has_pending && drag.c->sync_state != SYNC_WAITING) {
		uint64_t due = wm.last_drag_ms + WM_DRAG_INTERVAL_MS;
		/* 既に過ぎていても 0 は返さない。最低 1 ms で必ず前へ進む */
		best = (due > now_ms) ? (int)(due - now_ms) : 1;
	}

	if (drag.c->sync_state == SYNC_WAITING) {
		uint64_t due = drag.c->sync_sent_ms + WM_SYNC_TIMEOUT_MS;
		int ms = (due > now_ms) ? (int)(due - now_ms) : 1;
		if (best < 0 || ms < best)
			best = ms;
	}

	return best;
}

void move_end(bool cancel)
{
	struct client *c;
	struct rect final_geom;

	if (drag_client_gone())
		return;
	c = drag.c;

	xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);

	/* 先にセッションを畳む。以降の呼び出しが再入しても安全にする */
	final_geom       = drag.pending;
	drag.kind        = DRAG_NONE;
	drag.c           = NULL;
	drag.has_pending = false;

	if (cancel) {
		/* §7.2 _NET_WM_MOVERESIZE CANCEL / Esc: 開始前へ戻す */
		c->restore = drag.orig_restore;
		if (drag.unmaximized) {
			c->geom = drag.orig;
			layout_maximize(c,
			                (drag.orig_states & ST_MAXIMIZED_VERT) != 0,
			                (drag.orig_states & ST_MAXIMIZED_HORZ) != 0,
			                true);
		} else {
			c->geom = drag.orig;
			client_apply_geometry(c);
			client_send_configure(c);
		}
		xcb_flush(wm.conn);
		return;
	}

	/*
	 * §3.4.1-4 ドロップ時の確定。
	 * レート上限（-2）も未確定 1 件の規則（-3）もここでは解除する。
	 * これが無いと、ボタンを離してから最大 16 ms、あるいは §7.3 の
	 * 250 ms までウィンドウが最終位置・最終サイズにならず、
	 * 操作の終端が引っかかったように見える。
	 * 同期は §7.3「ボタン解放時の扱い」に従い、WAITING でも
	 * 新しい target を採番して即送る（古い通知は counter >= target で捨てる）。
	 */
	c->geom = final_geom;
	if (drag.edge != 0 || drag.orig.w != final_geom.w ||
	    drag.orig.h != final_geom.h)
		drag_sync_request(c, wm_now_ms());
	client_apply_geometry(c);
	client_send_configure(c);
	wm.last_drag_ms = wm_now_ms();

	/* §3.5.1 ユーザ起因のジオメトリ変更なので restore を更新する */
	drag_update_restore(c);

	xcb_flush(wm.conn);
}
