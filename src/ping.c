/*
 * ping.c - _NET_WM_PING による無応答検出と、起動通知 (SPEC §7.4)
 *
 * このファイルは 2 つの独立した機能を持つ:
 *
 *   1. _NET_WM_PING / 強制終了の方針
 *      WM がクライアントへ ping を送り、WM_PING_TIMEOUT_MS 以内に
 *      エコーが返らなければ「無応答」としてキャプションに印を付ける。
 *      強制終了の主手段は KillClient（xcb_kill_client）である。
 *      X 接続ごと切るので PID が要らず、リモート表示のクライアントにも効く。
 *      `_NET_WM_PID` を使った kill(2) は **WM_CLIENT_MACHINE がローカル
 *      ホスト名と一致する場合に限り**補助的に使ってよい —— PID は
 *      「表示しているホスト」ではなく「クライアントを実行しているホスト」の
 *      ものであり、一致を確認せずに signal を送ると、たまたま同じ番号を
 *      持つ無関係なローカルプロセスを殺しかねない（リモート X 接続では
 *      この事故が現実に起こる）。本ファイルは判定材料
 *      （ping_client_is_local）だけを提供し、実際にどちらを使うかの決定は
 *      client_close() 側に委ねる（下記「配線」参照）。
 *
 *   2. 起動通知 (startup notification)
 *      **ここが取り違えやすい点**（SPEC §7.4 が明示的に訂正している）:
 *      起動通知の実体は _NET_STARTUP_ID のような「プロパティ」ではない。
 *      ランチャがルートウィンドウ宛に送る
 *      `_NET_STARTUP_INFO_BEGIN` / `_NET_STARTUP_INFO` という
 *      **クライアントメッセージの列**であり、各メッセージは 20 バイト
 *      （ClientMessage の data8 全体）ずつに分割されたテキストの断片を運ぶ。
 *      受信側はこれを NUL に出会うまで連結し、できあがった 1 行を
 *      `new: ID="..." ...` / `remove: ID="..."` として解釈する。
 *      これらのアトム名は atoms.h の一括 intern テーブルには載っていない
 *      （読み書きする「プロパティ」が無いため、テーブルの設計はそのままでよい）。
 *      よって本ファイルが自前で 1 回だけ intern する（startup_atoms_ensure）。
 *      なお `_NET_STARTUP_ID` はこれとは別物で、こちらは実際に
 *      クライアントウィンドウ側のプロパティとして存在し、起動したアプリが
 *      「自分はどの起動通知に対応する窓か」を表明するのに使う
 *      （startup_window_mapped が読む）。
 *
 * 対応: SPEC §7.4
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "w98wm.h"

/* ================================================================== *
 * _NET_WM_PING
 * ================================================================== */

/*
 * 無応答キャプションの接尾辞 (SPEC §7.4)。
 *
 * deco.c の draw_title() は c->title / c->title_len をそのまま計測・描画
 * するだけで、「無応答かどうか」を一切知らない（deco.c にその概念は無い）。
 * deco.c を変更せずに済ませるため、この接尾辞は「見た目」として
 * c->title へ直接書き込む。これは _NET_WM_NAME / WM_NAME というプロパティ
 * そのものを書き換えるわけではない —— 応答が戻れば icccm_update_title() で
 * プロパティから読み直され、接尾辞は自然に消える (ping_handle_reply)。
 */
static const char PING_SUFFIX[] = " (応答なし)";

static void title_mark_unresponsive(struct client *c)
{
	size_t suf_len  = sizeof(PING_SUFFIX) - 1;
	size_t base_len = c->title_len;
	size_t room     = (size_t)(WM_TITLE_MAX - 1);   /* NUL の分を引く */

	if (base_len + suf_len > room) {
		/* 入り切らない。元のタイトルを UTF-8 境界で安全に削る */
		size_t keep = (room > suf_len) ? room - suf_len : 0;
		base_len = utf8_truncate_len(c->title, base_len, keep);
	}
	memcpy(c->title + base_len, PING_SUFFIX, suf_len);
	c->title[base_len + suf_len] = '\0';
	c->title_len = (uint8_t)(base_len + suf_len);

	/* 計測キャッシュの無効化 (§4.5.2.1)。icccm_update_title と同じ理由 */
	c->draw_glyphs = 0;
	c->caption_w_at_measure = 0;
	c->draw_ellipsis = 0;
}

/*
 * WM_CLIENT_MACHINE がローカルホスト名と一致するかどうか (SPEC §7.4)。
 *
 * kill(2) を _NET_WM_PID に対して使ってよいのはこの場合に限る。
 * 一致しない・プロパティが無い・ホスト名が取れない、のいずれも
 * 「わからない」であり、安全側に倒して false（= KillClient のみ）とする。
 *
 * 既知の限界: 単純な文字列完全一致であり、FQDN と短縮ホスト名の食い違い
 * （"host" と "host.example.com"）までは正規化しない。誤って true 側へ
 * 倒れることは「無関係なローカルプロセスを殺す」事故に直結するため、
 * ここは厳しめ（一致しないと false）に倒すのが正しい向きである。
 */
static bool ping_client_is_local(struct client *c)
{
	char machine[256];
	char hostname[256];
	size_t mlen;

	if (c == NULL)
		return false;

	mlen = prop_get_text(c->win, atoms[ATOM_WM_CLIENT_MACHINE],
	                     machine, sizeof machine);
	if (mlen == 0)
		return false;

	if (gethostname(hostname, sizeof hostname) != 0)
		return false;
	hostname[sizeof(hostname) - 1] = '\0';   /* POSIX: 切り詰め時の NUL 終端は保証されない */

	return strcmp(machine, hostname) == 0;
}

/* 応答確認を送る (SPEC §7.4)。CF_PING の無いクライアントには送らない。
 * 送信中の ping が既にあれば再送しない（二重に飛ばすと serial の対応が壊れる）。 */
void ping_client(struct client *c)
{
	xcb_client_message_event_t ev;

	if (c == NULL)
		return;
	if (!(c->flags & CF_PING))
		return;
	if (c->ping_sent_ms != 0)
		return;   /* 送信中のものがある */

	memset(&ev, 0, sizeof ev);
	ev.response_type  = XCB_CLIENT_MESSAGE;
	ev.format         = 32;
	ev.window         = c->win;
	ev.type           = atoms[ATOM_WM_PROTOCOLS];
	ev.data.data32[0] = atoms[ATOM_NET_WM_PING];
	ev.data.data32[1] = wm.last_time;   /* 慣例: このタイムスタンプをそのまま serial に使う */
	ev.data.data32[2] = c->win;

	xcb_send_event(wm.conn, 0, c->win, XCB_EVENT_MASK_NO_EVENT,
	              (const char *)&ev);

	c->ping_serial  = wm.last_time;
	c->ping_sent_ms = wm_now_ms();
}

/*
 * ping への応答 (SPEC §7.4)。
 *
 * クライアントは受け取った ClientMessage を「window フィールドだけ
 * root に差し替えて」送り返す（EWMH の規定）。したがって届いた時点の
 * ev->window は root であり、対象クライアントを引く鍵は
 * ev->window ではなく **data32[2]**（ping 送信時にこちらが c->win を
 * 詰めた場所）である。ここを ev->window で探すと絶対に見つからない。
 *
 * 戻り値は「ping 応答として消費したか」。type と ping アトムが一致した
 * 時点で true を返す —— 対象クライアントが既に居なくても、serial が
 * 古くても、それは ewmh_handle_client_message には渡すべきでない
 * メッセージであることに変わりはないため。
 */
bool ping_handle_reply(xcb_client_message_event_t *ev)
{
	struct client *c;
	xcb_window_t target;

	if (ev == NULL)
		return false;
	if (ev->type != atoms[ATOM_WM_PROTOCOLS] || ev->format != 32)
		return false;
	if (ev->data.data32[0] != atoms[ATOM_NET_WM_PING])
		return false;

	target = (xcb_window_t)ev->data.data32[2];
	c = client_find(target);
	if (c == NULL)
		return true;             /* 既に unmanage 済み。消費して終わり */

	if (c->ping_sent_ms == 0)
		return true;             /* 送信していない ping への応答。無視 */
	if (c->ping_serial != ev->data.data32[1])
		return true;             /* 古い ping への遅延応答 (§2.2.2 と同じ考え方) */

	c->ping_sent_ms = 0;
	if (c->unresponsive) {
		c->unresponsive = false;
		icccm_update_title(c);   /* プロパティから読み直し、接尾辞を落とす */
		deco_draw(c, NULL);
		LOG("ping: win=0x%08x が応答した。無応答表示を解除する (§7.4)",
		    (unsigned)c->win);
	}
	return true;
}

/*
 * WM_PING_TIMEOUT_MS 経過しても応答が無ければ無応答とみなす (SPEC §7.4)。
 * sync_check_timeout / startup_check_timeout と同じ「全クライアント走査」
 * パターン（sync.c 参照）。
 *
 * ここでは KillClient するかどうかまでは決めない。「強制終了の確認ダイアログ」
 * は Phase 4 の対象であり (client.c のコメント参照)、本関数の責務は
 * 「無応答状態を検出し、見える形にする」ところまでである。
 */
void ping_check_timeout(uint64_t now_ms)
{
	struct client *c;

	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->ping_sent_ms == 0)
			continue;
		if (c->unresponsive)
			continue;            /* 既に印を付け済み。応答を待つのみ */
		if (now_ms < c->ping_sent_ms)
			continue;            /* 単調時刻が巻き戻った。次回判定へ */
		if (now_ms - c->ping_sent_ms < (uint64_t)WM_PING_TIMEOUT_MS)
			continue;

		c->unresponsive = true;
		title_mark_unresponsive(c);
		if (c->flags & CF_DECORATED)
			deco_draw(c, NULL);

		LOG("ping: win=0x%08x が %u ms 応答せず。無応答として表示する"
		    "（強制終了時は %s。SPEC §7.4）",
		    (unsigned)c->win, (unsigned)WM_PING_TIMEOUT_MS,
		    ping_client_is_local(c)
		        ? "KillClient を主に用い、WM_CLIENT_MACHINE がローカルと"
		          "一致するため kill(2) も選択肢になる"
		        : "KillClient のみ（リモートまたはホスト名不明のため kill(2) は使わない）");
	}
}

/* ================================================================== *
 * 起動通知 (startup notification, SPEC §7.4)
 *
 * 実体はプロパティではなくルート宛のクライアントメッセージ列である点は
 * ファイル冒頭のコメントを参照。ここでは:
 *   - 同時に追跡する ID は最大 8 個（固定配列。動的確保はしない）
 *   - new: を受けたら記録して砂時計カーソルを表示する
 *   - remove:、対応する _NET_STARTUP_ID を持つ窓の map、または
 *     15 秒のタイムアウトのいずれかで個々の ID を解除する
 *   - 追跡中の ID が 0 個に戻ったら通常カーソルへ戻す
 * ================================================================== */

#define STARTUP_MAX_SEQ    8
#define STARTUP_TIMEOUT_MS 15000
#define STARTUP_ID_MAX     192   /* "app-PID-hostname/TIME-N" 相当に十分な余裕 */
#define STARTUP_MSG_MAX    512   /* new: 1 行分の累積バッファ（ID 以外の付随キーも含む） */

struct startup_seq {
	char     id[STARTUP_ID_MAX];
	uint64_t started_ms;
	bool     used;
};

static struct startup_seq g_seq[STARTUP_MAX_SEQ];
static bool                g_cursor_is_wait;

/* _NET_STARTUP_INFO(_BEGIN) 列から組み立て中の 1 メッセージ分の累積バッファ */
static char   g_accum[STARTUP_MSG_MAX];
static size_t g_accum_len;

/*
 * atoms.h の一括テーブルに乗っていない 3 アトムの遅延 intern。
 * atoms.c と同じ流儀（全部投げてからまとめて回収）で 1 往復に収める。
 * 初回のいずれかの呼び出し時に 1 度だけ行い、以後は往復しない
 * （取得に失敗しても再試行はしない。失敗時は起動通知機能ごと諦める）。
 */
static xcb_atom_t atom_startup_begin = XCB_ATOM_NONE;
static xcb_atom_t atom_startup_info  = XCB_ATOM_NONE;
static xcb_atom_t atom_startup_id    = XCB_ATOM_NONE;
static bool       g_startup_atoms_ready;

static void startup_atoms_ensure(void)
{
	xcb_intern_atom_cookie_t c1, c2, c3;
	xcb_intern_atom_reply_t *r;

	if (g_startup_atoms_ready)
		return;
	g_startup_atoms_ready = true;

	c1 = xcb_intern_atom(wm.conn, 0,
	    (uint16_t)(sizeof("_NET_STARTUP_INFO_BEGIN") - 1),
	    "_NET_STARTUP_INFO_BEGIN");
	c2 = xcb_intern_atom(wm.conn, 0,
	    (uint16_t)(sizeof("_NET_STARTUP_INFO") - 1),
	    "_NET_STARTUP_INFO");
	c3 = xcb_intern_atom(wm.conn, 0,
	    (uint16_t)(sizeof("_NET_STARTUP_ID") - 1),
	    "_NET_STARTUP_ID");

	r = xcb_intern_atom_reply(wm.conn, c1, NULL);
	if (r != NULL) { atom_startup_begin = r->atom; free(r); }
	r = xcb_intern_atom_reply(wm.conn, c2, NULL);
	if (r != NULL) { atom_startup_info = r->atom; free(r); }
	r = xcb_intern_atom_reply(wm.conn, c3, NULL);
	if (r != NULL) { atom_startup_id = r->atom; free(r); }
}

/* root のカーソルを、追跡中の ID の有無に合わせて更新する。
 * deco_set_cursor (deco.c) と同じ「変化した時だけ ChangeWindowAttributes
 * を投げる」流儀。 */
static void startup_update_cursor(void)
{
	bool any = false;
	int i;
	uint32_t vals[1];

	for (i = 0; i < STARTUP_MAX_SEQ; i++) {
		if (g_seq[i].used) {
			any = true;
			break;
		}
	}
	if (any == g_cursor_is_wait)
		return;

	g_cursor_is_wait = any;
	vals[0] = cursor_get(any ? CURSOR_WAIT : CURSOR_ARROW);
	xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_CURSOR, vals);
}

static void startup_add(const char *id, uint64_t now_ms)
{
	int i, free_slot = -1;

	for (i = 0; i < STARTUP_MAX_SEQ; i++) {
		if (g_seq[i].used && strcmp(g_seq[i].id, id) == 0) {
			g_seq[i].started_ms = now_ms;   /* 同じ ID の再通知。時計だけ更新 */
			return;
		}
		if (!g_seq[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		LOG("startup: 同時追跡数(%d)の上限に達した。new ID=%s を無視する (§7.4)",
		    STARTUP_MAX_SEQ, id);
		return;
	}

	strlcpy(g_seq[free_slot].id, id, sizeof g_seq[free_slot].id);
	g_seq[free_slot].started_ms = now_ms;
	g_seq[free_slot].used = true;
	startup_update_cursor();
}

static void startup_remove_by_id(const char *id)
{
	int i;

	for (i = 0; i < STARTUP_MAX_SEQ; i++) {
		if (g_seq[i].used && strcmp(g_seq[i].id, id) == 0) {
			g_seq[i].used = false;
			g_seq[i].id[0] = '\0';
		}
	}
	startup_update_cursor();
}

/*
 * "new: ID=\"...\" ..." / "remove: ID=\"...\"" から ID の値だけを取り出す。
 * libstartup-notification の実際のクォート規則（\" のエスケープ）に最小限
 * 対応する。ID を含まない・閉じクォートが無い等の壊れた入力は false を返す。
 */
static bool extract_id(const char *msg, char *out, size_t out_max)
{
	const char *p = strstr(msg, "ID=\"");
	size_t n = 0;

	if (p == NULL || out_max == 0)
		return false;
	p += 4;
	while (*p != '\0' && *p != '"' && n + 1 < out_max) {
		if (*p == '\\' && *(p + 1) != '\0')
			p++;   /* エスケープ文字自体は飛ばし、次の文字を素通しする */
		out[n++] = *p++;
	}
	out[n] = '\0';
	return n > 0;
}

static void startup_process_message(const char *msg)
{
	char id[STARTUP_ID_MAX];

	if (strncmp(msg, "new:", 4) == 0) {
		if (extract_id(msg, id, sizeof id))
			startup_add(id, wm_now_ms());
	} else if (strncmp(msg, "remove:", 7) == 0) {
		if (extract_id(msg, id, sizeof id))
			startup_remove_by_id(id);
	}
	/* change: 等その他のメッセージ種別は最小実装の対象外 (SPEC §7.4) */
}

/*
 * _NET_STARTUP_INFO_BEGIN / _NET_STARTUP_INFO クライアントメッセージ
 * (SPEC §7.4)。ルート宛の 20 バイト固定チャンクを連結し、NUL で 1 メッセージ
 * の区切りとする。戻り値は「起動通知として消費したか」。
 */
bool startup_handle_message(xcb_client_message_event_t *ev)
{
	const uint8_t *d;
	size_t i;
	bool begin;

	if (ev == NULL)
		return false;

	startup_atoms_ensure();

	if (ev->type == atom_startup_begin && atom_startup_begin != XCB_ATOM_NONE)
		begin = true;
	else if (ev->type == atom_startup_info && atom_startup_info != XCB_ATOM_NONE)
		begin = false;
	else
		return false;

	/* SPEC §7.4: ルートウィンドウ宛のメッセージ列。型は一致しているので
	 * ここで消費はするが、ルート以外へ送られたものは解釈しない。 */
	if (ev->window != wm.root || ev->format != 8)
		return true;

	if (begin)
		g_accum_len = 0;

	d = ev->data.data8;   /* ClientMessage の data8 は常に 20 バイト固定 */
	for (i = 0; i < 20; i++) {
		if (g_accum_len + 1 >= sizeof g_accum) {
			/* 想定外に長い／壊れた列。捨てて次のメッセージへ備える */
			g_accum_len = 0;
			return true;
		}
		if (d[i] == '\0') {
			if (g_accum_len > 0) {
				g_accum[g_accum_len] = '\0';
				startup_process_message(g_accum);
				g_accum_len = 0;
			}
			continue;   /* 残りは NUL パディング。読み捨てる */
		}
		g_accum[g_accum_len++] = (char)d[i];
	}
	return true;
}

/* 対応する _NET_STARTUP_ID を持つ窓が map された時点でも列を終える (SPEC §7.4) */
void startup_window_mapped(struct client *c)
{
	char id[STARTUP_ID_MAX];
	size_t len;

	if (c == NULL)
		return;

	startup_atoms_ensure();
	if (atom_startup_id == XCB_ATOM_NONE)
		return;

	len = prop_get_text(c->win, atom_startup_id, id, sizeof id);
	if (len == 0)
		return;

	startup_remove_by_id(id);
}

/* 15 秒経っても remove: も対応する窓の map も来ない ID をタイムアウトで解除する */
void startup_check_timeout(uint64_t now_ms)
{
	int i;
	bool changed = false;

	for (i = 0; i < STARTUP_MAX_SEQ; i++) {
		if (!g_seq[i].used)
			continue;
		if (now_ms < g_seq[i].started_ms)
			continue;   /* 時刻巻き戻り。次回判定へ */
		if (now_ms - g_seq[i].started_ms < (uint64_t)STARTUP_TIMEOUT_MS)
			continue;

		LOG("startup: ID=%s が %u ms 応答なし。タイムアウトで解除する (§7.4)",
		    g_seq[i].id, (unsigned)STARTUP_TIMEOUT_MS);
		g_seq[i].used = false;
		g_seq[i].id[0] = '\0';
		changed = true;
	}
	if (changed)
		startup_update_cursor();
}
