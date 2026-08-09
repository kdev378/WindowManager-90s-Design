/*
 * layout.c - モニタ検出、モニタ別作業領域、初期配置・最大化・全画面
 *
 * 対応する仕様: docs/SPEC.md §3.3.1, §3.5, §3.5.1, §3.5.2, §5.3, §7.2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

/* ================================================================== *
 * 内部ヘルパの前方宣言
 * ================================================================== */

struct rr_candidate {
	struct rect geom;
	xcb_atom_t  name_atom;
};

static void     monitor_add_candidate(struct rr_candidate *cand, uint8_t *n_cand,
                                       struct rect r, xcb_atom_t name_atom);
static void     monitor_set_name(struct monitor *m, xcb_atom_t name_atom, uint8_t idx);

static struct rect client_visible_rect(const struct client *c);

static struct monitor *pointer_monitor(void);
static void     clamp_to_workarea(struct client *c, const struct monitor *mon);
static void     center_on_monitor(struct client *c, const struct monitor *mon);
static void     cascade_place(struct client *c, struct monitor *mon);

static void     axis_horz_on(struct client *c, const struct monitor *mon);
static void     axis_horz_off(struct client *c);
static void     axis_vert_on(struct client *c, const struct monitor *mon);
static void     axis_vert_off(struct client *c);

/* ================================================================== *
 * layout_update_monitors — SPEC §5.3
 * ================================================================== */

/* ミラー出力の重複除去。既に同一ジオメトリの候補があれば追加しない (§5.3) */
static void monitor_add_candidate(struct rr_candidate *cand, uint8_t *n_cand,
                                   struct rect r, xcb_atom_t name_atom)
{
	if (!rect_valid(&r))
		return;
	if (*n_cand >= WM_MAX_MONITORS)
		return;

	for (uint8_t i = 0; i < *n_cand; i++) {
		if (memcmp(&cand[i].geom, &r, sizeof(struct rect)) == 0)
			return;
	}

	cand[*n_cand].geom = r;
	cand[*n_cand].name_atom = name_atom;
	(*n_cand)++;
}

/* MONITOR オブジェクトの name アトムを人間可読な文字列に解決する。取れなければ既定名 */
static void monitor_set_name(struct monitor *m, xcb_atom_t name_atom, uint8_t idx)
{
	m->name[0] = '\0';

	if (name_atom != XCB_NONE) {
		xcb_get_atom_name_cookie_t ck = xcb_get_atom_name(wm.conn, name_atom);
		xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(wm.conn, ck, NULL);
		if (r != NULL) {
			int len = xcb_get_atom_name_name_length(r);
			const char *nm = xcb_get_atom_name_name(r);
			size_t n = (size_t)len;
			if (n >= sizeof(m->name))
				n = sizeof(m->name) - 1;
			memcpy(m->name, nm, n);
			m->name[n] = '\0';
			free(r);
		}
	}

	if (m->name[0] == '\0')
		snprintf(m->name, sizeof(m->name), "Monitor %u", (unsigned)idx);
}

void layout_update_monitors(void)
{
	/* 更新前のモニタ一覧を退避。カスケード位置の引き継ぎ判定に使う */
	struct monitor old[WM_MAX_MONITORS];
	uint8_t n_old = wm.n_monitors;
	memcpy(old, wm.monitors, sizeof(struct monitor) * n_old);

	struct rr_candidate cand[WM_MAX_MONITORS];
	uint8_t n_cand = 0;
	bool got_any = false;

	if (wm.have_randr) {
		xcb_randr_query_version_cookie_t vck =
		    xcb_randr_query_version(wm.conn, 1, 5);
		xcb_randr_query_version_reply_t *vr =
		    xcb_randr_query_version_reply(wm.conn, vck, NULL);
		bool have_15 = vr != NULL &&
		    (vr->major_version > 1 ||
		     (vr->major_version == 1 && vr->minor_version >= 5));
		free(vr);

		if (have_15) {
			/* RandR 1.5: MONITOR オブジェクトを使う (§5.3) */
			xcb_randr_get_monitors_cookie_t mck =
			    xcb_randr_get_monitors(wm.conn, wm.root, 1);
			xcb_randr_get_monitors_reply_t *mr =
			    xcb_randr_get_monitors_reply(wm.conn, mck, NULL);
			if (mr != NULL) {
				xcb_randr_monitor_info_iterator_t it =
				    xcb_randr_get_monitors_monitors_iterator(mr);
				for (; it.rem > 0; xcb_randr_monitor_info_next(&it)) {
					xcb_randr_monitor_info_t *mi = it.data;
					struct rect r;
					r.x = mi->x;
					r.y = mi->y;
					r.w = mi->width;
					r.h = mi->height;
					monitor_add_candidate(cand, &n_cand, r, mi->name);
				}
				free(mr);
			}
			got_any = n_cand > 0;
		}

		if (!got_any) {
			/* RandR 1.2-1.4 フォールバック: CRTC を列挙する (§5.3) */
			xcb_randr_get_screen_resources_current_cookie_t sck =
			    xcb_randr_get_screen_resources_current(wm.conn, wm.root);
			xcb_randr_get_screen_resources_current_reply_t *sr =
			    xcb_randr_get_screen_resources_current_reply(wm.conn, sck, NULL);
			if (sr != NULL) {
				xcb_randr_crtc_t *crtcs =
				    xcb_randr_get_screen_resources_current_crtcs(sr);
				int n = xcb_randr_get_screen_resources_current_crtcs_length(sr);
				if (n > WM_MAX_MONITORS)
					n = WM_MAX_MONITORS;

				/* 全リクエストを先に送ってから回収する (§3.4.1 と同じ一括送信の原則) */
				xcb_randr_get_crtc_info_cookie_t ccks[WM_MAX_MONITORS];
				for (int i = 0; i < n; i++) {
					ccks[i] = xcb_randr_get_crtc_info(wm.conn, crtcs[i],
					    sr->config_timestamp);
				}
				for (int i = 0; i < n; i++) {
					xcb_randr_get_crtc_info_reply_t *ci =
					    xcb_randr_get_crtc_info_reply(wm.conn, ccks[i], NULL);
					if (ci == NULL)
						continue;
					/* モード無し、またはサイズ 0 の CRTC は無効として飛ばす (§5.3) */
					if (ci->mode == XCB_NONE || ci->width == 0 || ci->height == 0) {
						free(ci);
						continue;
					}
					struct rect r;
					r.x = ci->x;
					r.y = ci->y;
					r.w = ci->width;
					r.h = ci->height;
					monitor_add_candidate(cand, &n_cand, r, XCB_NONE);
					free(ci);
				}
				free(sr);
			}
			got_any = n_cand > 0;
		}
	}

	if (!got_any) {
		/* RandR が無い、または取得に失敗: スクリーン全体を単一モニタとして扱う (§5.3, §1.3) */
		struct rect r;
		r.x = 0;
		r.y = 0;
		r.w = wm.screen->width_in_pixels;
		r.h = wm.screen->height_in_pixels;
		n_cand = 0;
		monitor_add_candidate(cand, &n_cand, r, XCB_NONE);
	}

	uint8_t n_new = n_cand;
	bool fresh[WM_MAX_MONITORS];

	for (uint8_t i = 0; i < n_new; i++) {
		wm.monitors[i].geom = cand[i].geom;
		wm.monitors[i].workarea = cand[i].geom;
		monitor_set_name(&wm.monitors[i], cand[i].name_atom, i);

		fresh[i] = true;
		for (uint8_t j = 0; j < n_old; j++) {
			if (memcmp(&old[j].geom, &cand[i].geom, sizeof(struct rect)) == 0) {
				/* ジオメトリ不変のモニタはカスケード位置を維持する (§5.3 要件) */
				wm.monitors[i].cascade_x = old[j].cascade_x;
				wm.monitors[i].cascade_y = old[j].cascade_y;
				fresh[i] = false;
				break;
			}
		}
		if (fresh[i]) {
			wm.monitors[i].cascade_x = cand[i].geom.x;
			wm.monitors[i].cascade_y = cand[i].geom.y;
		}
	}
	wm.n_monitors = n_new;

	layout_update_workareas();

	/* 新規モニタのカスケード原点は作業領域確定後にその原点へ合わせ直す */
	for (uint8_t i = 0; i < n_new; i++) {
		if (fresh[i]) {
			wm.monitors[i].cascade_x = wm.monitors[i].workarea.x;
			wm.monitors[i].cascade_y = wm.monitors[i].workarea.y;
		}
	}
}

/* ================================================================== *
 * layout_update_workareas — SPEC §3.5.2
 * ================================================================== */

void layout_update_workareas(void)
{
	int32_t left_inset[WM_MAX_MONITORS];
	int32_t right_inset[WM_MAX_MONITORS];
	int32_t top_inset[WM_MAX_MONITORS];
	int32_t bottom_inset[WM_MAX_MONITORS];

	for (uint8_t i = 0; i < wm.n_monitors; i++) {
		left_inset[i] = right_inset[i] = top_inset[i] = bottom_inset[i] = 0;
		wm.monitors[i].workarea = wm.monitors[i].geom;
	}

	const int32_t screen_x0 = 0;
	const int32_t screen_y0 = 0;
	const int32_t screen_x1 = (int32_t)wm.screen->width_in_pixels;
	const int32_t screen_y1 = (int32_t)wm.screen->height_in_pixels;

	/*
	 * strut を持つのは管理下のクライアントだけではない。**内蔵タスクバーは
	 * override-redirect なので wm.stack_bottom には載らない** (§4.8)。
	 * ここを取りこぼすと自分のパネルの下にウィンドウが最大化される
	 * （実際にそうなっていた）。走査対象に自前のパネルを明示的に足す。
	 */
	{
	xcb_window_t extra = taskbar_strut_window();
	struct client *c = wm.stack_bottom;
	xcb_window_t cur;

	for (;;) {
		if (c != NULL) {
			cur = c->win;
		} else if (extra != XCB_WINDOW_NONE) {
			cur = extra;
			extra = XCB_WINDOW_NONE;
		} else {
			break;
		}

		int32_t left = 0, right = 0, top = 0, bottom = 0;
		/* strut のスパン。レガシー _NET_WM_STRUT は辺全体とみなす (§3.5.2) */
		int32_t lsy0 = screen_y0, lsy1 = screen_y1;
		int32_t rsy0 = screen_y0, rsy1 = screen_y1;
		int32_t tsx0 = screen_x0, tsx1 = screen_x1;
		int32_t bsx0 = screen_x0, bsx1 = screen_x1;
		bool have = false;

		uint32_t plen = 0;
		uint32_t *partial = prop_get_card32_list(cur,
		    atoms[ATOM_NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, &plen);
		if (partial != NULL && plen >= 12) {
			left   = (int32_t)partial[0];
			right  = (int32_t)partial[1];
			top    = (int32_t)partial[2];
			bottom = (int32_t)partial[3];
			lsy0 = (int32_t)partial[4];
			lsy1 = (int32_t)partial[5];
			rsy0 = (int32_t)partial[6];
			rsy1 = (int32_t)partial[7];
			tsx0 = (int32_t)partial[8];
			tsx1 = (int32_t)partial[9];
			bsx0 = (int32_t)partial[10];
			bsx1 = (int32_t)partial[11];
			have = true;
		}
		free(partial);

		if (!have) {
			uint32_t llen = 0;
			uint32_t *legacy = prop_get_card32_list(cur,
			    atoms[ATOM_NET_WM_STRUT], XCB_ATOM_CARDINAL, &llen);
			if (legacy != NULL && llen >= 4) {
				left   = (int32_t)legacy[0];
				right  = (int32_t)legacy[1];
				top    = (int32_t)legacy[2];
				bottom = (int32_t)legacy[3];
				have = true;
			}
			free(legacy);
		}

		if (!have || (left == 0 && right == 0 && top == 0 && bottom == 0)) {
			if (c != NULL)
				c = c->next;
			continue;
		}

		for (uint8_t i = 0; i < wm.n_monitors; i++) {
			struct monitor *m = &wm.monitors[i];
			int32_t mx0 = m->geom.x;
			int32_t mx1 = m->geom.x + (int32_t)m->geom.w;
			int32_t my0 = m->geom.y;
			int32_t my1 = m->geom.y + (int32_t)m->geom.h;

			/* 「接している」= モニタの当該辺がルートスクリーンの当該辺と一致 (§3.5.2) */
			if (left > 0 && mx0 == screen_x0 &&
			    lsy0 <= my1 - 1 && lsy1 >= my0 &&
			    left > left_inset[i]) {
				int32_t w = mx1 - (mx0 + left) - right_inset[i];
				if (w > 0)
					left_inset[i] = left; /* 0 以下になる strut は無視 (§3.5.2) */
			}
			if (right > 0 && mx1 == screen_x1 &&
			    rsy0 <= my1 - 1 && rsy1 >= my0 &&
			    right > right_inset[i]) {
				int32_t w = (mx1 - right) - (mx0 + left_inset[i]);
				if (w > 0)
					right_inset[i] = right;
			}
			if (top > 0 && my0 == screen_y0 &&
			    tsx0 <= mx1 - 1 && tsx1 >= mx0 &&
			    top > top_inset[i]) {
				int32_t h = my1 - (my0 + top) - bottom_inset[i];
				if (h > 0)
					top_inset[i] = top;
			}
			if (bottom > 0 && my1 == screen_y1 &&
			    bsx0 <= mx1 - 1 && bsx1 >= mx0 &&
			    bottom > bottom_inset[i]) {
				int32_t h = (my1 - bottom) - (my0 + top_inset[i]);
				if (h > 0)
					bottom_inset[i] = bottom;
			}
		}

		if (c != NULL)
			c = c->next;
	}
	}

	for (uint8_t i = 0; i < wm.n_monitors; i++) {
		struct monitor *m = &wm.monitors[i];
		m->workarea.x = (int16_t)(m->geom.x + left_inset[i]);
		m->workarea.y = (int16_t)(m->geom.y + top_inset[i]);
		m->workarea.w = (uint16_t)((int32_t)m->geom.w - left_inset[i] - right_inset[i]);
		m->workarea.h = (uint16_t)((int32_t)m->geom.h - top_inset[i] - bottom_inset[i]);
	}

	/* ルートの _NET_WORKAREA（外接矩形 1 個）は ewmh.c が持つ計算に委ねる */
	ewmh_update_workarea();
}

/* ================================================================== *
 * モニタ選択
 * ================================================================== */

struct monitor *layout_monitor_at(int16_t x, int16_t y)
{
	for (uint8_t i = 0; i < wm.n_monitors; i++) {
		if (rect_contains_point(&wm.monitors[i].geom, x, y))
			return &wm.monitors[i];
	}
	return NULL;
}

/* CSD クライアントは可視矩形 V = W - gtk_extents を基準にする (§7.2) */
static struct rect client_visible_rect(const struct client *c)
{
	struct rect v = c->geom;

	if (c->flags & CF_CSD) {
		int32_t l = c->gtk_extents[0];
		int32_t r = c->gtk_extents[1];
		int32_t t = c->gtk_extents[2];
		int32_t b = c->gtk_extents[3];
		int32_t nw = (int32_t)v.w - l - r;
		int32_t nh = (int32_t)v.h - t - b;
		if (nw > 0 && nh > 0) {
			v.x = (int16_t)(v.x + l);
			v.y = (int16_t)(v.y + t);
			v.w = (uint16_t)nw;
			v.h = (uint16_t)nh;
		}
	}
	return v;
}

struct monitor *layout_monitor_for(const struct client *c)
{
	struct rect v = client_visible_rect(c);
	int16_t cx, cy;
	rect_center(&v, &cx, &cy);

	struct monitor *m = layout_monitor_at(cx, cy);
	if (m != NULL)
		return m;

	/* 中心がどのモニタにも属さない: 交差面積が最大のモニタを選ぶ (§3.5) */
	struct monitor *best = NULL;
	uint32_t best_area = 0;
	for (uint8_t i = 0; i < wm.n_monitors; i++) {
		uint32_t area = rect_overlap_area(&v, &wm.monitors[i].geom);
		if (area > best_area) {
			best_area = area;
			best = &wm.monitors[i];
		}
	}
	if (best != NULL)
		return best;

	/* 交差が皆無ならポインタのあるモニタ、それも無ければモニタ 0 (§3.5) */
	struct monitor *pm = pointer_monitor();
	if (pm != NULL)
		return pm;

	return wm.n_monitors > 0 ? &wm.monitors[0] : NULL;
}

/* ポインタが乗っているモニタ。取得できなければモニタ 0（無ければ NULL） */
static struct monitor *pointer_monitor(void)
{
	xcb_query_pointer_cookie_t ck = xcb_query_pointer(wm.conn, wm.root);
	xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(wm.conn, ck, NULL);
	struct monitor *m = NULL;

	if (r != NULL) {
		m = layout_monitor_at(r->root_x, r->root_y);
		free(r);
	}
	if (m == NULL && wm.n_monitors > 0)
		m = &wm.monitors[0];
	return m;
}

/* ================================================================== *
 * layout_place_new — SPEC §3.3.1
 * ================================================================== */

/*
 * タイトルバー（フレーム上端の帯）が作業領域内に最低 1px 残るようクランプする。
 * ウィンドウが作業領域より大きい場合は縮めず左上をモニタ原点に合わせる (§3.3.1 手順4)。
 */
static void clamp_to_workarea(struct client *c, const struct monitor *mon)
{
	if (mon == NULL)
		return;

	const struct rect *wa = &mon->workarea;

	if (c->geom.w > wa->w)
		c->geom.x = wa->x;
	if (c->geom.h > wa->h)
		c->geom.y = wa->y;

	uint16_t left, right, top, bottom;
	client_frame_offsets(c, &left, &right, &top, &bottom);

	int32_t frame_x0 = (int32_t)c->geom.x - left;
	int32_t frame_w = (int32_t)c->geom.w + left + right;
	int32_t min_fx = (int32_t)wa->x - frame_w + 1;
	int32_t max_fx = (int32_t)wa->x + (int32_t)wa->w - 1;
	if (frame_x0 < min_fx)
		frame_x0 = min_fx;
	if (frame_x0 > max_fx)
		frame_x0 = max_fx;
	c->geom.x = (int16_t)(frame_x0 + left);

	int32_t frame_y0 = (int32_t)c->geom.y - top;
	int32_t min_fy = wa->y;
	int32_t max_fy = (int32_t)wa->y + (int32_t)wa->h - 1;
	if (frame_y0 < min_fy)
		frame_y0 = min_fy;
	if (frame_y0 > max_fy)
		frame_y0 = max_fy;
	c->geom.y = (int16_t)(frame_y0 + top);
}

static void center_on_monitor(struct client *c, const struct monitor *mon)
{
	if (mon == NULL)
		return;

	int16_t cx = (int16_t)(mon->workarea.x + mon->workarea.w / 2);
	int16_t cy = (int16_t)(mon->workarea.y + mon->workarea.h / 2);
	c->geom.x = (int16_t)(cx - c->geom.w / 2);
	c->geom.y = (int16_t)(cy - c->geom.h / 2);
}

/* モニタごとに独立したカスケード原点を持つ、簡易な巻き戻り段カウンタ (§3.3.1) */
static uint8_t cascade_round[WM_MAX_MONITORS];

static void cascade_place(struct client *c, struct monitor *mon)
{
	if (mon == NULL) {
		c->geom.x = 0;
		c->geom.y = 0;
		return;
	}

	const struct rect *wa = &mon->workarea;
	const int16_t step = 18; /* Win98 のキャプション高さ相当 (§3.3.1) */
	uint8_t idx = (uint8_t)(mon - wm.monitors);

	int16_t x = (int16_t)(mon->cascade_x + step);
	int16_t y = (int16_t)(mon->cascade_y + step);

	if ((int32_t)x + c->geom.w > (int32_t)wa->x + wa->w ||
	    (int32_t)y + c->geom.h > (int32_t)wa->y + wa->h) {
		/* 右下に到達: 原点へ戻し、開始オフセットを 1 段ずらす (§3.3.1) */
		if (idx < WM_MAX_MONITORS)
			cascade_round[idx] = (uint8_t)((cascade_round[idx] + 1) % 4);
		int16_t shift = (int16_t)((idx < WM_MAX_MONITORS ? cascade_round[idx] : 0) *
		    (step / 4));
		x = (int16_t)(wa->x + step + shift);
		y = (int16_t)(wa->y + step + shift);
	}

	mon->cascade_x = x;
	mon->cascade_y = y;
	c->geom.x = x;
	c->geom.y = y;
}

void layout_place_new(struct client *c)
{
	if (c->hints.flags & HINT_US_POSITION) {
		/* USPosition: 無条件で尊重し、クランプもしない (§3.3.1 手順1) */
		return;
	}

	bool have_pos = false;
	if (c->hints.flags & HINT_P_POSITION) {
		/* PPosition が (0,0) のときは「指定なし」として扱う (§3.3.1 手順2) */
		if (!(c->geom.x == 0 && c->geom.y == 0))
			have_pos = true;
	}

	if (have_pos) {
		/* 基準モニタは指定座標が属するモニタ（属さなければポインタのモニタ）(§3.3.1 手順5) */
		struct monitor *mon = layout_monitor_at(c->geom.x, c->geom.y);
		if (mon == NULL)
			mon = pointer_monitor();
		clamp_to_workarea(c, mon);
		return;
	}

	bool dialog_like = (c->type == TYPE_DIALOG) || (c->transient_for != XCB_NONE);

	if (dialog_like) {
		struct client *parent = NULL;
		if (c->transient_for != XCB_NONE && c->transient_for != wm.root)
			parent = client_find(c->transient_for);

		struct monitor *mon;
		if (parent != NULL) {
			/* 親の可視領域の中央 (§3.3.1 手順3, CSD は §7.2 の V を使用) */
			struct rect pv = client_visible_rect(parent);
			int16_t cx, cy;
			rect_center(&pv, &cx, &cy);
			mon = layout_monitor_at(cx, cy);
			if (mon == NULL) {
				/* 親が画面外: ポインタのあるモニタの中央に落とす (§3.3.1 手順3) */
				mon = pointer_monitor();
				center_on_monitor(c, mon);
				clamp_to_workarea(c, mon);
				return;
			}
			c->geom.x = (int16_t)(cx - c->geom.w / 2);
			c->geom.y = (int16_t)(cy - c->geom.h / 2);
		} else {
			/* 管理下の親が無い: ポインタのあるモニタの中央 (§3.3.1 手順3) */
			mon = pointer_monitor();
			center_on_monitor(c, mon);
		}
		clamp_to_workarea(c, mon);
		return;
	}

	if (c->type == TYPE_SPLASH) {
		struct monitor *mon = pointer_monitor();
		center_on_monitor(c, mon);
		clamp_to_workarea(c, mon);
		return;
	}

	/* それ以外（NORMAL 等）: Win98 流のカスケード配置 (§3.3.1 手順3) */
	struct monitor *mon = pointer_monitor();
	cascade_place(c, mon);
	clamp_to_workarea(c, mon);
}

/* ================================================================== *
 * layout_maximize / layout_fullscreen — SPEC §3.5, §3.5.1, §7.2
 * ================================================================== */

/*
 * 最大化ジオメトリは保存せず、呼び出しの都度モニタ別作業領域から導出する (§3.5.1)。
 * CSD クライアントは可視矩形 V が作業領域に一致するよう W = workarea + gtk_extents を
 * 設定する (§7.2)。
 */
/*
 * 最大化の基準は「**可視矩形**が作業領域と一致すること」(§3.5, §7.2)。
 *
 * SSD ではフレーム（ボーダー + キャプション）が可視矩形なので、
 * クライアント領域はその分だけ内側に置く。ここを取り違えて
 * `geom = workarea` にすると、フレームが作業領域を frame_offsets の分だけ
 * はみ出す。上辺のはみ出しは **キャプションが画面外へ出る**（最大化した窓の
 * タイトルバーが見えず、マウスで元に戻せない）という形で効き、
 * 下辺のはみ出しはタスクバーの上端を覆う。実際にそうなっていた。
 *
 * CSD では逆に _GTK_FRAME_EXTENTS（影と不可視ボーダー）の分だけ外へ広げる。
 * こちらは可視矩形が extents を差し引いた内側だからで、符号が逆になるのは
 * 意図どおり。
 */
static void frame_inset(const struct client *c, int32_t *l, int32_t *r,
                        int32_t *t, int32_t *b)
{
	uint16_t fl, fr, ft, fb;

	if (c->flags & CF_CSD) {
		*l = -(int32_t)c->gtk_extents[0];
		*r = -(int32_t)c->gtk_extents[1];
		*t = -(int32_t)c->gtk_extents[2];
		*b = -(int32_t)c->gtk_extents[3];
		return;
	}
	client_frame_offsets(c, &fl, &fr, &ft, &fb);
	*l = fl;
	*r = fr;
	*t = ft;
	*b = fb;
}

static void axis_horz_on(struct client *c, const struct monitor *mon)
{
	int32_t l, r, t, b;
	int32_t w;

	frame_inset(c, &l, &r, &t, &b);
	c->geom.x = (int16_t)((int32_t)mon->workarea.x + l);
	w = (int32_t)mon->workarea.w - l - r;
	c->geom.w = (uint16_t)(w > 1 ? w : 1);
}

static void axis_horz_off(struct client *c)
{
	c->geom.x = c->restore.x;
	c->geom.w = c->restore.w;
}

static void axis_vert_on(struct client *c, const struct monitor *mon)
{
	int32_t l, r, t, b;
	int32_t h;

	frame_inset(c, &l, &r, &t, &b);
	c->geom.y = (int16_t)((int32_t)mon->workarea.y + t);
	h = (int32_t)mon->workarea.h - t - b;
	c->geom.h = (uint16_t)(h > 1 ? h : 1);
}

static void axis_vert_off(struct client *c)
{
	c->geom.y = c->restore.y;
	c->geom.h = c->restore.h;
}

void layout_maximize(struct client *c, bool vert, bool horz, bool on)
{
	struct monitor *mon = layout_monitor_for(c);
	bool shaded = (c->states & ST_SHADED) != 0;

	if (horz) {
		if (on) {
			if (mon != NULL)
				axis_horz_on(c, mon);
			c->states |= ST_MAXIMIZED_HORZ;
		} else {
			axis_horz_off(c);
			c->states &= ~(uint32_t)ST_MAXIMIZED_HORZ;
		}
	}

	if (vert) {
		/*
		 * シェード中に最大化した場合はシェードを維持したまま幅だけを最大化し、
		 * restore.h を保持する。ここでは高さに触れないだけでよい。restore は
		 * このファイルからは一切書き換えないため、保持は自動的に満たされる (§3.5.1)。
		 */
		if (!shaded) {
			if (on) {
				if (mon != NULL)
					axis_vert_on(c, mon);
			} else {
				axis_vert_off(c);
			}
		}
		if (on)
			c->states |= ST_MAXIMIZED_VERT;
		else
			c->states &= ~(uint32_t)ST_MAXIMIZED_VERT;
	}

	client_apply_geometry(c);
	ewmh_set_wm_state(c);
	ewmh_set_allowed_actions(c);
	/*
	 * CSD の再計算は allowed_actions の後（Motif の制限を上書きしないため）、
	 * stack_apply の前。CSD でなければ即 return するので SSD には影響しない (§7.2)。
	 */
	csd_state_changed(c);
	stack_apply();
}

void layout_fullscreen(struct client *c, bool on)
{
	if (on) {
		if (c->states & ST_FULLSCREEN)
			return;

		if (c->states & ST_SHADED) {
			/*
			 * シェード中に全画面要求が来たら先にシェードを解除する (§3.5.1)。
			 * shade 専用の公開 API がヘッダに無いため、ここで直接復元する。
			 */
			c->states &= ~(uint32_t)ST_SHADED;
			c->geom.h = c->restore.h;
		}

		/* 全画面に入る直前のジオメトリと状態を保存する (§3.5.1) */
		c->pre_fs = c->geom;
		c->pre_fs_states = c->states;

		/* 作業用ジオメトリからは最大化ビットを落とす (§3.5.1) */
		c->states &= ~(uint32_t)(ST_MAXIMIZED_VERT | ST_MAXIMIZED_HORZ);
		c->states |= ST_FULLSCREEN;

		struct monitor *mon = layout_monitor_for(c);
		if (mon != NULL)
			c->geom = mon->geom; /* strut 無視、モニタ全域 (§3.5) */

		c->flags &= ~(uint32_t)CF_DECORATED; /* 装飾を消す (§3.5) */
	} else {
		if (!(c->states & ST_FULLSCREEN))
			return;

		c->geom = c->pre_fs;
		c->states = c->pre_fs_states;

		/*
		 * 最大化ジオメトリは保存しない原則（§3.5.1）を全画面解除後にも適用する。
		 * 直前に最大化されていた軸は、pre_fs の数値ではなく現在のモニタ別作業領域
		 * から改めて導出し、全画面中のモニタ構成変更にも追従させる。
		 */
		struct monitor *mon = layout_monitor_for(c);
		if (mon != NULL) {
			if (c->states & ST_MAXIMIZED_HORZ)
				axis_horz_on(c, mon);
			if (c->states & ST_MAXIMIZED_VERT)
				axis_vert_on(c, mon);
		}

		c->flags &= ~(uint32_t)CF_DECORATED;
		if (client_decides_decoration(c))
			c->flags |= CF_DECORATED;
	}

	client_apply_geometry(c);
	ewmh_set_wm_state(c);
	ewmh_set_allowed_actions(c);
	/*
	 * GTK は全画面時に _GTK_FRAME_EXTENTS を 0 にするため、遷移のたびに
	 * 読み直して可視矩形 V を基準に再計算する (§7.2)。
	 * allowed_actions の後に置くのは Motif の制限を上書きしないため。
	 */
	csd_state_changed(c);
	stack_apply();
}
