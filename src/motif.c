/*
 * motif.c - _MOTIF_WM_HINTS と GTK クライアントサイド装飾 (CSD)
 *
 * 対応する仕様: docs/SPEC.md §1.1.1, §3.2, §3.5, §3.5.1, §7.2
 *
 * ──────────────────────────────────────────────────────────────────
 * 【最初に読むこと】CSD ウィンドウが Win98 の外観にならないのは仕様である
 * ──────────────────────────────────────────────────────────────────
 * GTK3/4 の CSD アプリは自前でタイトルバーを描く。本 WM は §7.2 の規定どおり
 * そこへ装飾を付けない。結果として **CSD アプリだけは Win98 の外観にならない**。
 * これは §1.1.1 が「実装で解決する課題ではなく、この設計を選んだ時点で確定する
 * 限界」として明示的に受け入れた項目であり、バグではない。
 *
 * したがって将来この挙動を「直す」ためにここへ装飾を足してはならない。
 * 足せば GTK 自身のヘッダバーと Win98 のキャプションが二重に並ぶ
 * （それを承知で行うのが force_ssd = best-effort モード。§7.2、後述）。
 * 外観の一致を要求できるのは SSD を使うアプリ（Qt・Firefox・xterm・
 * 多くの GTK3 アプリ）に限る、というのが本仕様の立場である。
 *
 * ──────────────────────────────────────────────────────────────────
 * このファイルの守備範囲
 * ──────────────────────────────────────────────────────────────────
 *   motif_update()      _MOTIF_WM_HINTS を 1 往復で読み、装飾可否を
 *                       client->motif_seen / motif_no_deco へキャッシュする。
 *                       functions は _NET_WM_ALLOWED_ACTIONS へ反映する。
 *   csd_state_changed() 最大化・全画面・復元の遷移、および
 *                       _GTK_FRAME_EXTENTS の PropertyNotify で呼ぶ。
 *                       extents を読み直し、可視矩形 V が意図した位置に
 *                       来るようウィンドウ実矩形 W を解き直す。
 *
 * 用語 (§7.2): ウィンドウ実矩形 W、extents (l, r, t, b) に対し
 *              利用者に見える矩形 V = W − (l, r, t, b)。
 *              配置・最大化・全画面はすべて V を基準に決める。
 *
 * エラー方針 (§2.2.2): クライアント宛の GetProperty はリプライが要るので
 * checked 形で出し、BadWindow 等はこの場で握ってイベントループへ漏らさない。
 * xcb のリプライは全経路で free する。
 */
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

#include "w98wm.h"

/* ================================================================== *
 * _MOTIF_WM_HINTS の定義 (§3.2)
 *
 * Motif のヘッダ (Xm/MwmUtil.h) は依存に入れられないため、プロトコル上の
 * 値をここで定義する。プロパティは CARD32 5 語:
 *   [0] flags  [1] functions  [2] decorations  [3] input_mode  [4] status
 * ================================================================== */

#define MWM_HINTS_LEN            5u

/* flags */
#define MWM_HINTS_FUNCTIONS      (1u << 0)
#define MWM_HINTS_DECORATIONS    (1u << 1)

/* functions */
#define MWM_FUNC_ALL             (1u << 0)
#define MWM_FUNC_RESIZE          (1u << 1)
#define MWM_FUNC_MOVE            (1u << 2)
#define MWM_FUNC_MINIMIZE        (1u << 3)
#define MWM_FUNC_MAXIMIZE        (1u << 4)
#define MWM_FUNC_CLOSE           (1u << 5)

#define MWM_FUNC_KNOWN \
	(MWM_FUNC_RESIZE | MWM_FUNC_MOVE | MWM_FUNC_MINIMIZE | \
	 MWM_FUNC_MAXIMIZE | MWM_FUNC_CLOSE)

/* ================================================================== *
 * プロパティの読み取り
 * ================================================================== */

/*
 * _MOTIF_WM_HINTS を 1 往復で読む。
 *
 * 型を照合しない（XCB_GET_PROPERTY_TYPE_ANY）のは意図的である。
 * 型として _MOTIF_WM_HINTS アトム自身を書くクライアントと CARDINAL を
 * 書くクライアントが両方存在し、厳密に照合するとヒントを取りこぼす。
 * 一方 format == 32 は必須とする（format 8/16 で書かれた壊れた値を
 * CARD32 として読むとゴミが decorations に入り、装飾が消える）。
 *
 * prop_get_card32_list() を使わないのは (a) 型を固定してしまうこと、
 * (b) 長さを知るために 2 往復すること、の 2 点による。語数は 5 で固定なので
 * 最初から 5 語要求すれば 1 往復で済む。
 *
 * 戻り値: out へ実際に書いた語数。プロパティが無い・短い・format 不一致なら 0。
 * 短いプロパティ（クライアントは 1 バイトだけ設定することもできる）でも
 * 落ちないよう、書けた分だけを返して呼び出し側に判断させる。
 */
static size_t motif_read_hints(xcb_window_t win, uint32_t *out, size_t want)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_generic_error_t *err = NULL;
	const uint32_t *src;
	size_t got, i;

	for (i = 0; i < want; i++)
		out[i] = 0;

	cookie = xcb_get_property(wm.conn, 0, win, atoms[ATOM_MOTIF_WM_HINTS],
	                          XCB_GET_PROPERTY_TYPE_ANY, 0, (uint32_t)want);
	reply = xcb_get_property_reply(wm.conn, cookie, &err);
	if (reply == NULL) {
		free(err);                 /* BadWindow 等はここで握る (§2.2.2) */
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
	for (i = 0; i < got; i++)
		out[i] = src[i];

	free(reply);
	return got;
}

/* ================================================================== *
 * MWM_HINTS_FUNCTIONS → _NET_WM_ALLOWED_ACTIONS (§3.2)
 * ================================================================== */

/*
 * 採用した対応表。EWMH に対応物のある 5 機能だけを写し、それ以外
 * （SHADE / FULLSCREEN / CHANGE_DESKTOP / STICK）には触れない。
 * Motif には対応する機能ビットが無く、「無効」と読むと EWMH 側の
 * 意味を勝手に狭めてしまうためである。
 *
 *   MWM_FUNC_MOVE     → _NET_WM_ACTION_MOVE
 *   MWM_FUNC_RESIZE   → _NET_WM_ACTION_RESIZE
 *   MWM_FUNC_MINIMIZE → _NET_WM_ACTION_MINIMIZE
 *   MWM_FUNC_MAXIMIZE → _NET_WM_ACTION_MAXIMIZE_HORZ と _VERT の両方
 *                       （Motif は軸を区別しないので両方を同時に落とす）
 *   MWM_FUNC_CLOSE    → _NET_WM_ACTION_CLOSE
 *
 * ビットの意味（Motif の規約。ここを取り違えると意味が反転する）:
 *   MWM_FUNC_ALL が立っている  … 列挙されたビットは「除外する機能」
 *   MWM_FUNC_ALL が立っていない … 列挙されたビットが「許可する機能」
 *
 * 制限はあくまで **通知** である。_NET_WM_ALLOWED_ACTIONS は EWMH 上
 * 「WM が受け付ける操作の広告」であり、WM 自身の操作を禁じる規定は §6 に
 * 無い。ここで実際のキーバインドやボタンを殺すことはしない
 * （殺すと Motif ヒントを雑に書くアプリが閉じられなくなる）。
 */
static uint32_t motif_allowed_funcs(const uint32_t *h, size_t n)
{
	uint32_t f;

	if (n < 2 || (h[0] & MWM_HINTS_FUNCTIONS) == 0)
		return MWM_FUNC_KNOWN;      /* 指定なし = 全部許可 */

	f = h[1];
	if (f & MWM_FUNC_ALL)
		return MWM_FUNC_KNOWN & ~f; /* 列挙されたものを除外 */
	return MWM_FUNC_KNOWN & f;          /* 列挙されたものだけ許可 */
}

/*
 * _NET_WM_ALLOWED_ACTIONS を Motif の functions で絞って書き出す。
 *
 * 基本の顔ぶれ（MOVE/CLOSE は常時、CF_FIXED_SIZE でなければ
 * RESIZE と MAXIMIZE_*、タスクバーに出る種別なら MINIMIZE）は
 * ewmh_set_allowed_actions() と同じ規則をなぞっている。
 *
 * ここで二重定義になっているのは、ヘッダの struct client に Motif の
 * functions を持つ場所が無く、ewmh_set_allowed_actions() から参照できない
 * ためである。恒久的な解は struct client に functions を持たせて
 * ewmh_set_allowed_actions() 側で絞ることであり、その場合この関数は消える。
 * （現状の副作用: 他モジュールが ewmh_set_allowed_actions() を呼ぶと
 *   絞り込みは上書きされ、次に motif_update() / csd_state_changed() が
 *   走るまで戻らない。）
 */
static void motif_apply_allowed_actions(struct client *c)
{
	uint32_t h[MWM_HINTS_LEN];
	size_t n;
	uint32_t allow;
	xcb_atom_t list[6];
	uint32_t k = 0;

	n = motif_read_hints(c->win, h, MWM_HINTS_LEN);
	allow = motif_allowed_funcs(h, n);

	if (allow == MWM_FUNC_KNOWN) {
		/* 制限なし。素の規則をそのまま使う（規則の重複を避ける） */
		ewmh_set_allowed_actions(c);
		return;
	}

	if (allow & MWM_FUNC_MOVE)
		list[k++] = atoms[ATOM_NET_WM_ACTION_MOVE];
	if (allow & MWM_FUNC_CLOSE)
		list[k++] = atoms[ATOM_NET_WM_ACTION_CLOSE];

	if (!(c->flags & CF_FIXED_SIZE)) {
		if (allow & MWM_FUNC_RESIZE)
			list[k++] = atoms[ATOM_NET_WM_ACTION_RESIZE];
		if (allow & MWM_FUNC_MAXIMIZE) {
			list[k++] = atoms[ATOM_NET_WM_ACTION_MAXIMIZE_HORZ];
			list[k++] = atoms[ATOM_NET_WM_ACTION_MAXIMIZE_VERT];
		}
	}

	if (type_props(c->type)->in_taskbar && (allow & MWM_FUNC_MINIMIZE))
		list[k++] = atoms[ATOM_NET_WM_ACTION_MINIMIZE];

	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->win,
	                    atoms[ATOM_NET_WM_ALLOWED_ACTIONS],
	                    XCB_ATOM_ATOM, 32, k, list);
}

/* ================================================================== *
 * motif_update — _MOTIF_WM_HINTS の読み取りとキャッシュ (§3.2)
 * ================================================================== */

/*
 * client_decides_decoration() は PropertyNotify のたびに呼ばれる。
 * そこで毎回 GetProperty を往復させないよう、結果を
 * client->motif_seen / motif_no_deco へ寄せる。
 * 更新の入口はこの関数だけ（管理開始時と _MOTIF_WM_HINTS の PropertyNotify）。
 */
void motif_update(struct client *c)
{
	static bool force_ssd_logged;   /* §7.2 の警告は 1 回だけ出す */
	uint32_t h[MWM_HINTS_LEN];
	size_t n;
	bool no_deco;

	if (c == NULL)
		return;

	n = motif_read_hints(c->win, h, MWM_HINTS_LEN);

	/*
	 * decorations が意味を持つのは
	 *   ・3 語以上読めた（flags と decorations が揃っている）
	 *   ・flags に MWM_HINTS_DECORATIONS が立っている
	 * の両方が成り立つときだけ。1 バイトしか設定しない壊れたクライアントは
	 * ここで n < 3 に落ち、「ヒント無し」と同じ扱いになる。
	 *
	 * §3.2 は「MWM_DECOR_ALL が無効、または decorations = 0」と書くが、
	 * MWM_DECOR_ALL (1<<0) は「列挙したものを除外する」という反転指示子で
	 * あって装飾の有無ではない。decorations == 0 は MWM_DECOR_ALL も
	 * 立っていない状態なので、0 判定だけで両方を覆える。
	 */
	no_deco = false;
	if (n >= 3 && (h[0] & MWM_HINTS_DECORATIONS) != 0 && h[2] == 0)
		no_deco = true;

	c->motif_seen = (n > 0);
	c->motif_no_deco = no_deco;

	/*
	 * force_ssd は _MOTIF_WM_HINTS を無視して装飾を強制する best-effort
	 * モード (§7.2)。CSD 相手ではタイトルバーが二重になるため推奨しない。
	 * 実際に効いた場面で 1 回だけ記録し、GTK_CSD=0 の案内を添える。
	 */
	if (wm.cfg.force_ssd && (no_deco || (c->flags & CF_CSD)) &&
	    !force_ssd_logged) {
		force_ssd_logged = true;
		/* -v の有無に関わらず出す。設定の副作用（装飾の二重化）を
		 * 利用者が知らないまま使い続けるのを避けるため (§7.2)。 */
		ERR("force_ssd: overriding the client's undecorated request "
		    "(SPEC 7.2, best-effort, not recommended). "
		    "For GTK apps prefer GTK_CSD=0 in the environment.");
	}

	/* functions の反映 (§3.2)。装飾の再判定は呼び出し側の責務 */
	motif_apply_allowed_actions(c);
}

/* ================================================================== *
 * 幾何ヘルパ — client_visual_rect() の逆変換 (§7.2)
 * ================================================================== */

/* int16 / uint16 への飽和。W = workarea + extents は容易に範囲外へ出る */
static int16_t sat_i16(int32_t v)
{
	if (v < -32768) return (int16_t)-32768;
	if (v >  32767) return (int16_t)32767;
	return (int16_t)v;
}

static uint16_t sat_dim(int32_t v)
{
	if (v < 1)     return 1;       /* X は幅・高さ 0 を許さない (BadValue) */
	if (v > 65535) return (uint16_t)65535;
	return (uint16_t)v;
}

/*
 * 「可視矩形を v にしたい」から、設定すべきウィンドウ実矩形 W
 * （= struct client.geom）を解く。client_visual_rect() のちょうど逆である。
 *
 *   CSD : V = W − (l, r, t, b)  →  W = V + (l, r, t, b)
 *   SSD : V = フレーム外形       →  W = V を装飾の厚みぶん内側へ寄せた矩形
 *
 * 可視矩形そのものの計算は client.c の client_visual_rect() が唯一の実装で
 * あり、ここで再実装してはならない（§7.2 が 1 箇所への集約を要求している）。
 * この関数はその逆向きだけを提供する。
 *
 * static なのはヘッダが宣言していないため。最大化の軸計算 (layout.c の
 * axis_horz_on / axis_vert_on) は同じ式を各自で持っており、本来は
 * この関数を共有すべきである（詳細は報告を参照）。
 */
static void csd_window_rect_for_visible(const struct client *c,
                                        const struct rect *v,
                                        struct rect *out)
{
	int32_t l, r, t, b;

	if (c->flags & CF_CSD) {
		l = (int32_t)c->gtk_extents[0];
		r = (int32_t)c->gtk_extents[1];
		t = (int32_t)c->gtk_extents[2];
		b = (int32_t)c->gtk_extents[3];

		out->x = sat_i16((int32_t)v->x - l);
		out->y = sat_i16((int32_t)v->y - t);
		out->w = sat_dim((int32_t)v->w + l + r);
		out->h = sat_dim((int32_t)v->h + t + b);
		return;
	}

	{
		uint16_t ul, ur, ut, ub;

		client_frame_offsets(c, &ul, &ur, &ut, &ub);
		out->x = sat_i16((int32_t)v->x + (int32_t)ul);
		out->y = sat_i16((int32_t)v->y + (int32_t)ut);
		out->w = sat_dim((int32_t)v->w - (int32_t)ul - (int32_t)ur);
		out->h = sat_dim((int32_t)v->h - (int32_t)ut - (int32_t)ub);
	}
}

static bool rect_equal(const struct rect *a, const struct rect *b)
{
	return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

/* ================================================================== *
 * csd_state_changed — 状態遷移と extents 変化への追従 (§7.2)
 * ================================================================== */

/*
 * 呼ぶ場面（§7.2「状態遷移時」の行）:
 *   ・最大化 / 全画面 / 復元の遷移が終わったところ
 *   ・_GTK_FRAME_EXTENTS の PropertyNotify
 *
 * 手順の順序には理由がある。
 *
 * 1. 先に extents を読み直す。GTK は全画面に入ると影を消して
 *    _GTK_FRAME_EXTENTS を 0 に更新するため、遷移前にキャッシュした値で
 *    計算すると必ず外す（§7.2 が名指しで禁じている先読み）。
 *    逆に、遷移直後はまだ 0 に更新されていないこともある。どちらの順序でも
 *    正しく収束するよう、PropertyNotify でも同じ経路を通す。
 *
 * 2. 装飾の可否を引き直す。CSD には装飾を付けない（§3.2 / §7.2）。
 *    判定は client_decides_decoration() に一任する。CF_CSD を見落とした
 *    独自判定をここに書くと「Win98 のフレーム + GTK のヘッダバー」という
 *    二重装飾になる。
 *
 * 3. 現在の状態に応じて V の目標を決め、W を解き直す。
 *    全画面 → V == モニタ全域、最大化 → V == モニタ別作業領域（軸ごと）。
 *
 * 4. _NET_FRAME_EXTENTS を書き直す。CSD なら (0,0,0,0) になる
 *    （WM 側は何も描かないため。§7.2）。ewmh_set_frame_extents() が
 *    CF_CSD を見て 0 を書くので、ここで値を組み立てはしない。
 */
void csd_state_changed(struct client *c)
{
	struct rect target, want;
	struct monitor *mon;
	bool recompute = false;

	if (c == NULL)
		return;

	/* --- 1. 必ず読み直す (§7.2) --- */
	icccm_update_gtk_extents(c);

	/* --- 2. 装飾の再判定 (§3.2, §7.2, §1.1.1) --- */
	c->flags &= ~(uint32_t)CF_DECORATED;
	if (client_decides_decoration(c))
		c->flags |= CF_DECORATED;

	/*
	 * --- 3. ジオメトリの解き直し ---
	 *
	 * 触るのは CSD クライアントだけに限る。SSD の最大化・全画面ジオメトリは
	 * layout.c が所有しており、ここから重ねて書き換えると所有者が二人になる。
	 * （SSD 側にも V 基準への寄せが要る箇所はあるが、それは layout.c の
	 *   修正であって本ファイルの担当ではない。報告に回す。）
	 */
	if ((c->flags & CF_CSD) == 0)
		goto done;

	mon = layout_monitor_for(c);
	if (mon == NULL)
		goto done;

	client_visual_rect(c, &target);   /* 現在の V。触らない軸はこのまま使う */

	if (c->states & ST_FULLSCREEN) {
		/*
		 * 全画面: V がモニタ全域と一致するようにする (§3.5, §7.2)。
		 * 1. で extents を読み直した **後** に解くこと。
		 * 遷移前の非ゼロな値で引くと、全画面なのに四辺へ影のぶんだけ
		 * 隙間が残る（= 仕様が防ごうとしている症状そのもの）。
		 * strut は無視してモニタ全域を使う (§3.5)。
		 */
		target = mon->geom;
		recompute = true;
	} else if (c->states & (ST_MAXIMIZED_VERT | ST_MAXIMIZED_HORZ)) {
		/*
		 * 最大化: V がモニタ別作業領域と一致するよう W = workarea + extents。
		 * VERT と HORZ は独立した状態なので軸ごとに当てる (§3.5.1)。
		 * シェード中は高さに触れない（幅だけ最大化し restore.h を保つ。§3.5.1）。
		 */
		if (c->states & ST_MAXIMIZED_HORZ) {
			target.x = mon->workarea.x;
			target.w = mon->workarea.w;
		}
		if ((c->states & ST_MAXIMIZED_VERT) &&
		    !(c->states & ST_SHADED)) {
			target.y = mon->workarea.y;
			target.h = mon->workarea.h;
		}
		recompute = true;
	}
	/*
	 * 通常状態では W に触れない。
	 * WM 側が課す目標が無いためで、かつ GTK は影の付け外しの際に
	 * _GTK_FRAME_EXTENTS とウィンドウサイズを一組で変更してくる。
	 * ここで「V を保つ」補正を重ねると二重に効いて V がずれる。
	 */

	if (!recompute)
		goto done;

	csd_window_rect_for_visible(c, &target, &want);
	if (!rect_equal(&want, &c->geom)) {
		bool moved_only = (want.w == c->geom.w && want.h == c->geom.h);

		c->geom = want;
		client_apply_geometry(c);
		/*
		 * 大きさが変わらず位置だけ動いた場合、子の frame 内相対位置は
		 * 不変なので実 ConfigureNotify が飛ばない。ルート相対座標を
		 * 使うクライアントが古い値を持ち続けるため明示的に送る
		 * （ICCCM §4.1.5、client.c の client_send_configure() の注記）。
		 */
		if (moved_only)
			client_send_configure(c);
	}

done:
	/* --- 4. _NET_FRAME_EXTENTS。CSD は (0,0,0,0) (§7.2) --- */
	ewmh_set_frame_extents(c);

	/*
	 * 遷移のたびに Motif の functions 制限を貼り直す。
	 * layout_maximize() / layout_fullscreen() は素の
	 * ewmh_set_allowed_actions() を呼ぶため、この関数を遷移の最後に
	 * 置かないと制限が消えたままになる（配置の意図は報告を参照）。
	 */
	motif_apply_allowed_actions(c);
}
