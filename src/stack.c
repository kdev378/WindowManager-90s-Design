/*
 * stack.c - スタッキング順の管理 (SPEC §3.7, §3.7.1, §3.7.2)
 *
 * wm.stack_bottom / wm.stack_top は「下から上」の実体リスト（双方向）。
 * ここでの並びはユーザ操作（raise/lower/新規マップ）に由来する入力順であり、
 * 実際に X へ発行する最終順序は stack_apply() が毎回このリストから
 * レイヤ・transient チェーンを考慮して計算し直す（このリスト自体は書き換えない）。
 */
#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * _NET_CLIENT_LIST 用の age（マップ順）リスト。
 * client 構造体に順序フィールドを追加できない（w98wm.h は変更禁止）ため、
 * stack.c 内部だけで別管理する。stack_add() で末尾に追加、
 * stack_remove() で該当要素を詰めて削除する。
 * ------------------------------------------------------------------ */
static struct client *g_order[WM_MAX_CLIENTS];
static uint16_t       g_order_n;

/* ------------------------------------------------------------------ *
 * stack_apply() の作業領域。呼び出し頻度が高いため malloc せず
 * ファイルスコープの static 配列を使い回す (要求どおり)。
 * ------------------------------------------------------------------ */
static struct client *g_visible[WM_MAX_CLIENTS];
static uint8_t        g_eff_layer[WM_MAX_CLIENTS];

static struct client *g_sorted[WM_MAX_CLIENTS];
static uint8_t        g_sorted_layer[WM_MAX_CLIENTS];
static uint16_t       g_ns;              /* g_sorted の有効長 */

static bool            g_emitted[WM_MAX_CLIENTS];
static struct client  *g_final[WM_MAX_CLIENTS];
static uint16_t        g_final_n;        /* stack_update_client_list() と共有 */

/* ==================================================================== *
 * リスト（stack_bottom/top）の素の連結操作
 * ==================================================================== */

static void list_unlink(struct client *c)
{
	if (c->prev) c->prev->next = c->next; else wm.stack_bottom = c->next;
	if (c->next) c->next->prev = c->prev; else wm.stack_top = c->prev;
	c->prev = c->next = NULL;
}

static void list_link_top(struct client *c)
{
	c->prev = wm.stack_top;
	c->next = NULL;
	if (wm.stack_top) wm.stack_top->next = c; else wm.stack_bottom = c;
	wm.stack_top = c;
}

static void list_link_bottom(struct client *c)
{
	c->next = wm.stack_bottom;
	c->prev = NULL;
	if (wm.stack_bottom) wm.stack_bottom->prev = c; else wm.stack_top = c;
	wm.stack_bottom = c;
}

void stack_add(struct client *c)
{
	if (!c) return;

	c->prev = c->next = NULL;
	list_link_top(c);
	wm.n_clients++;

	/* age 順（_NET_CLIENT_LIST 用）。WM_MAX_CLIENTS を超える分は
	 * そもそも「装飾なしで野に放つ」対象 (§2.3.0) なので掲載漏れを許容する。 */
	if (g_order_n < WM_MAX_CLIENTS)
		g_order[g_order_n++] = c;
}

void stack_remove(struct client *c)
{
	if (!c) return;

	list_unlink(c);
	if (wm.n_clients > 0) wm.n_clients--;

	for (uint16_t i = 0; i < g_order_n; i++) {
		if (g_order[i] == c) {
			for (uint16_t j = i; j + 1 < g_order_n; j++)
				g_order[j] = g_order[j + 1];
			g_order_n--;
			break;
		}
	}
}

void stack_raise(struct client *c)
{
	if (!c || wm.stack_top == c) return;
	list_unlink(c);
	list_link_top(c);
	/* transient の子を親の直上に保つ処理は stack_apply() が呼ばれるたびに
	 * チェーンを再計算して行う (§3.7.2) ので、ここでは自分自身の位置だけ
	 * 動かせば十分。次の stack_apply() でレイヤ内の最上位に来る。 */
}

void stack_lower(struct client *c)
{
	if (!c || wm.stack_bottom == c) return;
	list_unlink(c);
	list_link_bottom(c);
}

/* ==================================================================== *
 * transient チェーンの解決 (SPEC §3.7.2)
 * ==================================================================== */

struct client *stack_transient_parent(struct client *c)
{
	if (!c) return NULL;

	xcb_window_t tf = c->transient_for;
	if (tf == XCB_NONE) return NULL;

	/* ICCCM: transient_for がルートを指す場合は「グループ全体への transient」。
	 * 単一の親を持たないので呼び出し側にはグループ扱いを促す (§3.7.2)。 */
	if (tf == wm.root) return NULL;

	/* 自己参照はここで打ち切る。壊れた/悪意あるクライアントが
	 * transient_for に自分自身を指すことがある (§3.7.2)。 */
	if (tf == c->win) return NULL;

	return client_find(tf);
}

bool stack_is_transient_of(struct client *child, struct client *ancestor)
{
	if (!child || !ancestor) return false;

	/* 循環検出込みの探索。A→B→A のようなループや深いチェーンで
	 * ハングしないよう、訪問済み集合と WM_TRANSIENT_DEPTH 段の
	 * 打ち切りを併用する (§3.7.2)。 */
	struct client *visited[WM_TRANSIENT_DEPTH];
	int n_visited = 0;

	struct client *cur = stack_transient_parent(child);
	while (cur && n_visited < WM_TRANSIENT_DEPTH) {
		for (int i = 0; i < n_visited; i++) {
			if (visited[i] == cur)
				return false; /* 循環を検出。ancestor には到達しないとみなす */
		}
		visited[n_visited++] = cur;

		if (cur == ancestor)
			return true;

		cur = stack_transient_parent(cur);
	}
	return false;
}

/* ウィンドウ種別・状態ビットから決まる「自分自身の」レイヤ（親の影響を含まない）。
 * SPEC §3.7.1。 */
static uint8_t own_layer(struct client *c)
{
	uint8_t layer = type_props(c->type)->layer;

	if (c->states & ST_BELOW) layer = LAYER_BELOW;
	if (c->states & ST_ABOVE) layer = LAYER_DOCK;

	/* フルスクリーンは「フォーカスウィンドウが自分自身、または自分の
	 * transient チェーンに属する」ときだけ最上層 (§3.7.1)。
	 * 動画プレイヤの設定ダイアログにフォーカスが移っても本体を
	 * LAYER_NORMAL へ落とさないための例外。effective_layer 側の
	 * 「子を親の層へ引き上げる」規則では親を維持できないため、
	 * ここで別途判定する必要がある。 */
	if ((c->states & ST_FULLSCREEN) &&
	    (wm.focused == c || stack_is_transient_of(wm.focused, c))) {
		layer = LAYER_FULLSCREEN;
	}

	return layer;
}

uint8_t stack_effective_layer(struct client *c)
{
	if (!c) return LAYER_NORMAL;

	/* effective_layer(c) = max(layer_of(c), effective_layer(parent)) を
	 * 再帰ではなく反復で評価する。再帰にすると循環時に無限再帰で
	 * ハングするため、既訪問集合 + WM_TRANSIENT_DEPTH 段で打ち切る
	 * のを1箇所にまとめている (§3.7.2)。 */
	struct client *visited[WM_TRANSIENT_DEPTH];
	int n_visited = 0;
	uint8_t best = 0;

	struct client *cur = c;
	while (cur && n_visited < WM_TRANSIENT_DEPTH) {
		bool seen = false;
		for (int i = 0; i < n_visited; i++) {
			if (visited[i] == cur) { seen = true; break; }
		}
		if (seen) break;
		visited[n_visited++] = cur;

		uint8_t l = own_layer(cur);
		if (l > best) best = l;

		cur = stack_transient_parent(cur);
	}
	return best;
}

/* ==================================================================== *
 * stack_apply() — 最終順序の計算と実際の ConfigureWindow 発行
 * ==================================================================== */

static bool visible_on_current_desktop(const struct client *c)
{
	return c->desktop == wm.current_desktop ||
	       c->desktop == WM_ALL_DESKTOPS ||
	       (c->states & ST_STICKY) != 0;
}

/* g_sorted[i] の「隣接引き上げ」対象となる親のインデックス。無ければ -1。
 * 同一レイヤ内の親のみを対象にする（レイヤが異なる場合は既にレイヤ
 * バケットの並びだけで子が親より上に来るため、隣接させる意味がない）。
 * 壊れた transient_for によって親候補が実は自分の子孫だった場合は
 * 循環になるので採用しない (§3.7.2)。 */
static int32_t parent_index(uint16_t i)
{
	struct client *p = stack_transient_parent(g_sorted[i]);
	if (!p) return -1;

	for (uint16_t j = 0; j < g_ns; j++) {
		if (g_sorted[j] != p) continue;
		if (g_sorted_layer[j] != g_sorted_layer[i]) return -1;
		if (stack_is_transient_of(g_sorted[j], g_sorted[i])) return -1; /* 循環防止 */
		return (int32_t)j;
	}
	return -1;
}

/* g_sorted[i] とその transient の子孫を、親の直上に来るよう g_final へ
 * 詰めていく。g_emitted による二重登録防止が循環に対する最終防波堤にもなる。 */
static void emit(uint16_t i)
{
	if (g_emitted[i]) return;
	g_emitted[i] = true;
	g_final[g_final_n++] = g_sorted[i];

	for (uint16_t j = 0; j < g_ns; j++) {
		if (g_emitted[j]) continue;
		if (parent_index(j) == (int32_t)i)
			emit(j);
	}
}

void stack_apply(void)
{
	/* 1. 現在のデスクトップに見えているクライアントだけを対象にする
	 *    (§3.8: STICKY と WM_ALL_DESKTOPS は常に対象)。 */
	uint16_t nv = 0;
	for (struct client *c = wm.stack_bottom; c && nv < WM_MAX_CLIENTS; c = c->next) {
		if (!visible_on_current_desktop(c)) continue;
		g_visible[nv] = c;
		g_eff_layer[nv] = stack_effective_layer(c);
		nv++;
	}

	/* 2. レイヤ番号でバケットソート。同一レイヤ内は元の相対順を保つ
	 *    (安定ソート = §3.7.2 「transient は同一レイヤ内での順序付け」の前提)。 */
	g_ns = 0;
	for (uint8_t layer = 0; layer < LAYER_COUNT; layer++) {
		for (uint16_t i = 0; i < nv; i++) {
			if (g_eff_layer[i] != layer) continue;
			g_sorted[g_ns] = g_visible[i];
			g_sorted_layer[g_ns] = layer;
			g_ns++;
		}
	}

	/* 3. 各 transient チェーンを親の直上に引き寄せる。 */
	g_final_n = 0;
	for (uint16_t i = 0; i < g_ns; i++) g_emitted[i] = false;
	for (uint16_t i = 0; i < g_ns; i++) {
		if (parent_index(i) == -1)
			emit(i);
	}

	/* 4. 実際の ConfigureWindow を、下から上へ隣接ウィンドウ基準で発行する。
	 *    N 回の独立した raise ではなく、1 パスの連鎖で発行すること。 */
	for (uint16_t i = 1; i < g_final_n; i++) {
		uint32_t values[2];
		values[0] = g_final[i - 1]->frame;
		values[1] = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(wm.conn, g_final[i]->frame,
		                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
		                     values);
	}

	stack_update_client_list();
}

void stack_update_client_list(void)
{
	static xcb_window_t buf[WM_MAX_CLIENTS];
	uint16_t n;

	/* _NET_CLIENT_LIST: マップされた順（age 順）。クライアントウィンドウの配列。 */
	n = 0;
	for (uint16_t i = 0; i < g_order_n && n < WM_MAX_CLIENTS; i++)
		buf[n++] = g_order[i]->win;
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
	                    atoms[ATOM_NET_CLIENT_LIST], XCB_ATOM_WINDOW, 32,
	                    n, buf);

	/* _NET_CLIENT_LIST_STACKING: 直近の stack_apply() が計算した実スタック順。 */
	n = 0;
	for (uint16_t i = 0; i < g_final_n && n < WM_MAX_CLIENTS; i++)
		buf[n++] = g_final[i]->win;
	xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
	                    atoms[ATOM_NET_CLIENT_LIST_STACKING], XCB_ATOM_WINDOW, 32,
	                    n, buf);
}
