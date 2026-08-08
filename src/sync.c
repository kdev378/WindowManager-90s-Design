/*
 * sync.c - _NET_WM_SYNC_REQUEST によるリサイズの同期 (SPEC §7.3)
 *
 * これが無いと GTK/Qt/Electron のリサイズが激しくちらつく。
 * 本ファイルは §7.3 の状態機械そのものであり、ペーシング（§3.4.1）の側は
 * move.c が持つ。move.c は「WAITING なら送らずに保留」「250ms で STALLED」
 * という判定だけを行い、カウンタとアラームの面倒はすべてここで見る。
 *
 * 実装する範囲（取り違えやすいので明記する）:
 *   本ファイルは **基本プロトコル（カウンタ 1 個）のみ**を実装する。
 *   値は単調増加するリクエスト ID であって偶奇に意味は無い。
 *   偶数／奇数でフレームの途中経過を表すのはカウンタ 2 個の拡張プロトコル
 *   （extended frame sync）であり、別物である。プロパティに 2 個入っていた
 *   場合は 1 個目だけを使う（§7.3）。
 *
 * ヘッダのワート（§7.3 の設計レビューで指摘済み・ヘッダは変更しない）:
 *   enum sync_state の SYNC_IDLE が 0 なので、memset(0) しただけの
 *   struct client は「カウンタが無いのに IDLE」に見える。
 *   したがって **カウンタに触る経路はすべて sync_counter != XCB_NONE を
 *   併せて確認する**。本ファイルの各関数はこの規約を守っている。
 */

#include <stdlib.h>
#include <string.h>

#include <xcb/sync.h>

#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * 64bit カウンタ値の取り扱い (§7.3)
 *
 * カウンタは 64bit（hi/lo の 2 ワード）である。32bit で扱って桁上がりを
 * 落とすと、以後アラームが二度と発火しない。
 * ------------------------------------------------------------------ */

/* WM 側の target（非負の単調増加値）を X の INT64 表現へ落とす */
static void u64_split(uint64_t v, uint32_t *hi, uint32_t *lo)
{
	*hi = (uint32_t)(v >> 32);
	*lo = (uint32_t)(v & 0xFFFFFFFFu);
}

/*
 * AlarmNotify が報告したカウンタ値が target 以上か。
 * X の INT64 は符号付きなので、hi < 0（負のカウンタ）は常に「未達」とする。
 * 負値を符号なしとして解釈すると巨大な値に化け、未更新のカウンタを
 * 「充足した」と誤判定する。
 */
static bool int64_ge_u64(xcb_sync_int64_t v, uint64_t target)
{
	uint64_t u;

	if (v.hi < 0)
		return false;
	u = ((uint64_t)(uint32_t)v.hi << 32) | (uint64_t)v.lo;
	return u >= target;
}

/* ------------------------------------------------------------------ *
 * クライアントの探索
 *
 * スタックリストは client_manage が必ず登録する（client.c: stack_add）ので、
 * 管理下のクライアントはすべてここを走査すれば見つかる。
 * 台数は WM_MAX_CLIENTS = 512 が上限で、AlarmNotify は 1 コマに高々数件しか
 * 来ないため、線形探索で足りる（専用の索引は持たない）。
 * ------------------------------------------------------------------ */

static struct client *find_by_alarm(xcb_sync_alarm_t alarm)
{
	struct client *c;

	if (alarm == XCB_NONE)
		return NULL;
	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->sync_alarm == alarm)
			return c;
	}
	return NULL;
}

static struct client *find_by_sync_resource(uint32_t res)
{
	struct client *c;

	if (res == XCB_NONE)
		return NULL;
	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->sync_alarm == res || c->sync_counter == res)
			return c;
	}
	return NULL;
}

/* ------------------------------------------------------------------ *
 * カウンタ無しへの縮退
 *
 * §7.3:「カウンタを持たないクライアントは最初から STALLED と同じ扱い」。
 * クライアントが自分のカウンタを先に破棄した場合（§2.2.2 の許容エラー）も
 * ここへ落とす。致命扱いにはしない。
 * ------------------------------------------------------------------ */
static void sync_drop(struct client *c)
{
	if (c->sync_alarm != XCB_NONE) {
		/* 自分のリソースなので破棄は必ず行う。カウンタが先に消えていれば
		 * BadAlarm になりうるが、それは sync_handle_event 側で握る。 */
		xcb_sync_destroy_alarm(wm.conn, c->sync_alarm);
		c->sync_alarm = XCB_NONE;
	}
	c->sync_counter = XCB_NONE;
	c->sync_target  = 0;
	c->sync_sent_ms = 0;
	c->sync_state   = SYNC_NONE;
}

/* ------------------------------------------------------------------ *
 * アラームの生成
 *
 * 値リストは「マスクのビットの昇順」に並べる。CA_VALUE(4) は CA_TEST_TYPE(8)
 * より前であり、しかも INT64 なので hi/lo の 2 ワードを占める。
 * §7.3 のコード片はマスクを COUNTER|VALUE_TYPE|TEST_TYPE|VALUE|EVENTS の
 * 順に書いているが、あれは可読性のための並びであって、ワイヤ上の順序ではない。
 * ここを取り違えると test_type と value が入れ替わり、アラームが無言になる。
 * ------------------------------------------------------------------ */
static bool sync_alarm_create(struct client *c)
{
	uint32_t list[8];
	uint32_t mask;
	uint32_t hi, lo;
	xcb_void_cookie_t ck;
	xcb_generic_error_t *err;

	/*
	 * delta を 0 で明示する。既定は 1 で、その場合サーバは発火のたびに
	 * trigger を +1 して自動再武装するため、+1 ずつ進む使い方だと
	 * ChangeAlarm を忘れても「たまたま」動いてしまう（反証実験で確認）。
	 * クライアントのカウンタが先へ飛ぶと通知を落とすので、
	 * 偶然の正しさに寄りかからないよう delta=0 に固定し、
	 * 再武装は必ず ChangeAlarm で明示的に行う。
	 * 属性リストはマスクのビット値の昇順に並べること
	 * （CA_VALUE(4) が CA_TEST_TYPE(8) より先。順序を誤ると無言で発火しなくなる）。
	 */
	mask = XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE |
	       XCB_SYNC_CA_VALUE   | XCB_SYNC_CA_TEST_TYPE |
	       XCB_SYNC_CA_DELTA   | XCB_SYNC_CA_EVENTS;

	u64_split(1u, &hi, &lo);          /* 初期の trigger 値は 1 (§7.3) */

	/*
	 * 並びはマスクのビット値の昇順:
	 *   COUNTER(1) / VALUE_TYPE(2) / VALUE(4: hi,lo) /
	 *   TEST_TYPE(8) / DELTA(16: hi,lo) / EVENTS(32)
	 * VALUE と DELTA は 64bit なので **2 ワードずつ**占める点に注意。
	 * ここを 1 ワードで数えると以降が全てずれ、無言で発火しなくなる。
	 */
	list[0] = (uint32_t)c->sync_counter;
	list[1] = (uint32_t)XCB_SYNC_VALUETYPE_ABSOLUTE;
	list[2] = hi;                     /* VALUE.hi */
	list[3] = lo;                     /* VALUE.lo */
	list[4] = (uint32_t)XCB_SYNC_TESTTYPE_POSITIVE_COMPARISON;
	list[5] = 0u;                     /* DELTA.hi = 0 */
	list[6] = 0u;                     /* DELTA.lo = 0 (自動再武装させない) */
	list[7] = 1u;                     /* events = true */

	c->sync_alarm = xcb_generate_id(wm.conn);

	/*
	 * ここだけ checked にする。クライアントが既にカウンタを破棄していると
	 * XSyncBadCounter になり、§2.2.2 に従って「カウンタ無し」へ落とす必要が
	 * あるためで、往復 1 回は管理開始時（およびプロパティ差し替え時）にしか
	 * 起きない。§3.4.1 が禁じているのは描画経路に往復を置くことであり、
	 * ここは描画経路ではない。
	 */
	ck  = xcb_sync_create_alarm_checked(wm.conn, c->sync_alarm, mask, list);
	err = xcb_request_check(wm.conn, ck);
	if (err != NULL) {
		LOG("sync: CreateAlarm 失敗 (code=%u res=0x%08x)。"
		    "カウンタ無しとして続行する (§2.2.2)",
		    (unsigned)err->error_code, (unsigned)err->resource_id);
		free(err);
		c->sync_alarm = XCB_NONE;
		return false;
	}
	return true;
}

/* ================================================================== *
 * 公開 API
 * ================================================================== */

/*
 * _NET_WM_SYNC_REQUEST_COUNTER を読み、アラームを 1 個作る。
 * カウンタがあれば SYNC_IDLE、無ければ SYNC_NONE。
 */
void sync_client_init(struct client *c)
{
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *r;
	const uint32_t *v;
	uint32_t n;

	if (c == NULL)
		return;

	c->sync_counter = XCB_NONE;
	c->sync_alarm   = XCB_NONE;
	c->sync_target  = 0;
	c->sync_sent_ms = 0;
	c->sync_state   = SYNC_NONE;   /* 既定は「カウンタ無し」＝ STALLED 相当 */

	if (!wm.have_sync)
		return;

	/*
	 * 2 個目まで読む。2 個入っているのは拡張プロトコル（別物）を名乗る
	 * クライアントで、§7.3 に従い **1 個目のみ**を使う。
	 * long_length=2 なので往復は 1 回で済む。
	 */
	ck = xcb_get_property(wm.conn, 0, c->win,
	                      atoms[ATOM_NET_WM_SYNC_REQUEST_COUNTER],
	                      XCB_ATOM_CARDINAL, 0, 2);
	r  = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return;   /* 取得までの間に消えた。カウンタ無し扱い (§2.2.2) */

	if (r->type != XCB_ATOM_CARDINAL || r->format != 32 ||
	    xcb_get_property_value_length(r) < (int)sizeof(uint32_t)) {
		free(r);
		return;
	}

	v = xcb_get_property_value(r);
	n = (uint32_t)(xcb_get_property_value_length(r) / (int)sizeof(uint32_t));
	c->sync_counter = (xcb_sync_counter_t)v[0];
	if (n >= 2) {
		LOG("sync: win=0x%08x はカウンタを %u 個提示した。"
		    "基本プロトコルとして 1 個目のみ使う (§7.3)",
		    (unsigned)c->win, (unsigned)n);
	}
	free(r);

	if (c->sync_counter == XCB_NONE)
		return;

	if (!sync_alarm_create(c)) {
		c->sync_counter = XCB_NONE;
		return;
	}

	c->sync_state = SYNC_IDLE;
	LOG("sync: win=0x%08x counter=0x%08x alarm=0x%08x",
	    (unsigned)c->win, (unsigned)c->sync_counter,
	    (unsigned)c->sync_alarm);
}

/*
 * サーバ側リソースの解放 (§7.3 / §9.1 指標 C)。
 * 忘れるとアラームがウィンドウ数に比例して残る。通常操作では表面化しない。
 */
void sync_client_fini(struct client *c)
{
	if (c == NULL)
		return;
	if (c->sync_alarm != XCB_NONE) {
		xcb_sync_destroy_alarm(wm.conn, c->sync_alarm);
		c->sync_alarm = XCB_NONE;
	}
	c->sync_counter = XCB_NONE;
	c->sync_target  = 0;
	c->sync_sent_ms = 0;
	c->sync_state   = SYNC_NONE;
}

/*
 * _NET_WM_SYNC_REQUEST_COUNTER が差し替えられた（実クライアントで起きる）。
 * 古いアラームを破棄して作り直す (§7.3)。
 * CF_SYNC_UNFIT はセッション中の記録なので、ここでは解除しない
 * （解除は「実際にカウンタが充足した」ときだけ。sync_handle_event を参照）。
 */
void sync_property_changed(struct client *c)
{
	if (c == NULL)
		return;
	if (c->sync_alarm != XCB_NONE) {
		xcb_sync_destroy_alarm(wm.conn, c->sync_alarm);
		c->sync_alarm = XCB_NONE;
	}
	/*
	 * 差し替えの瞬間に WAITING だった場合、古いカウンタからの通知は
	 * もう来ない。sync_client_init が状態を IDLE か NONE に置き直すので、
	 * move.c 側の「未確定 1 件」の閂は自動的に外れる（＝固まらない）。
	 */
	sync_client_init(c);
}

/*
 * リサイズ 1 コマの開始 (§7.3)。
 * 戻り値 false は「同期を使わない」＝呼び出し側はそのまま非同期で進めてよい。
 */
bool sync_request(struct client *c)
{
	xcb_client_message_event_t ev;
	uint32_t list[2];
	uint32_t hi, lo;

	if (c == NULL || !wm.have_sync)
		return false;
	/*
	 * SYNC_IDLE == 0 というヘッダのワートの防波堤。
	 * 状態だけを見るとゼロ初期化のクライアントが IDLE に見えるため、
	 * カウンタ／アラームの実在を必ず併せて確認する。
	 */
	if (c->sync_counter == XCB_NONE || c->sync_alarm == XCB_NONE)
		return false;
	if (c->sync_state == SYNC_NONE)
		return false;
	/*
	 * §7.3: 一度でもタイムアウトしたクライアントはセッション中「同期不適合」。
	 * これが無いと、カウンタを持つが更新しないアプリで **ドラッグを始めるたびに
	 * 250 ms の待ち**が入り、操作の出だしが毎回引っかかる。
	 */
	if (c->flags & CF_SYNC_UNFIT)
		return false;

	c->sync_target++;                 /* 単調増加するリクエスト ID */
	u64_split(c->sync_target, &hi, &lo);

	/*
	 * ★ このモジュールで最も忘れやすい 1 行 (§7.3) ★
	 * POSITIVE_COMPARISON は「カウンタ値 >= trigger 値」で発火する。
	 * trigger を作った時のまま放置すると、最初の 1 回だけ発火して以後は
	 * 永久に沈黙し、**すべてのリサイズが 250 ms の STALLED 経路へ静かに
	 * 縮退する**（動くように見えて遅い、という最悪の壊れ方をする）。
	 * よってリクエストのたびに trigger を新しい target へ更新する。
	 * 値リストは CA_VALUE の 1 項目だけなので hi, lo の順の 2 ワード。
	 * ここは描画経路なので unchecked（往復を作らない。§3.4.1 補足）。
	 */
	list[0] = hi;
	list[1] = lo;
	xcb_sync_change_alarm(wm.conn, c->sync_alarm, XCB_SYNC_CA_VALUE, list);

	/* WM_PROTOCOLS 型の ClientMessage で target を伝える */
	memset(&ev, 0, sizeof ev);
	ev.response_type  = XCB_CLIENT_MESSAGE;
	ev.format         = 32;
	ev.sequence       = 0;
	ev.window         = c->win;
	ev.type           = atoms[ATOM_WM_PROTOCOLS];
	ev.data.data32[0] = atoms[ATOM_NET_WM_SYNC_REQUEST];
	ev.data.data32[1] = (uint32_t)wm.last_time;
	ev.data.data32[2] = lo;          /* target の下位 32bit */
	ev.data.data32[3] = hi;          /* target の上位 32bit */
	ev.data.data32[4] = 0;

	xcb_send_event(wm.conn, 0 /* propagate */, c->win,
	               XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);

	c->sync_sent_ms = wm_now_ms();
	c->sync_state   = SYNC_WAITING;
	return true;
}

/*
 * XSync のイベント（と XSync 宛リクエストのエラー）を処理する。
 * 処理したら true。
 */
bool sync_handle_event(xcb_generic_event_t *ev)
{
	const xcb_sync_alarm_notify_event_t *an;
	struct client *c;

	if (ev == NULL || !wm.have_sync)
		return false;

	/*
	 * §2.2.2 / §7.3: カウンタ由来のエラー（XSyncBadAlarm / XSyncBadCounter）は
	 * 異常ではない。クライアントは終了時に自分のカウンタを破棄し、それは
	 * こちらのアラーム破棄より先に起こりうる。致命扱いにせず、そのクライアントを
	 * 「カウンタ無し」へ落として続行する。
	 *
	 * 判定にはエラーコードではなく major_opcode を使う。struct wm には
	 * first_event（sync_base）しか無く first_error が保持されていないため、
	 * エラーコードから XSync のエラーを引き当てる術が無い（報告事項）。
	 * 拡張の major opcode は xcb 側でキャッシュ済みなので往復は生じない。
	 */
	if (ev->response_type == 0) {
		const xcb_generic_error_t *err = (const xcb_generic_error_t *)ev;
		const xcb_query_extension_reply_t *ext =
		    xcb_get_extension_data(wm.conn, &xcb_sync_id);

		if (ext == NULL || !ext->present ||
		    err->major_code != ext->major_opcode)
			return false;      /* XSync のエラーではない。event.c に任せる */

		c = find_by_sync_resource(err->resource_id);
		if (c != NULL) {
			LOG("sync: XSync エラー code=%u res=0x%08x。"
			    "win=0x%08x をカウンタ無しへ落とす (§2.2.2)",
			    (unsigned)err->error_code,
			    (unsigned)err->resource_id, (unsigned)c->win);
			sync_drop(c);
		} else {
			LOG("sync: 持ち主不明の XSync エラー code=%u res=0x%08x を無視",
			    (unsigned)err->error_code, (unsigned)err->resource_id);
		}
		return true;
	}

	if ((ev->response_type & 0x7Fu) !=
	    (uint8_t)(wm.sync_base + XCB_SYNC_ALARM_NOTIFY))
		return false;

	an = (const xcb_sync_alarm_notify_event_t *)ev;
	c  = find_by_alarm(an->alarm);
	if (c == NULL)
		return true;   /* 既に unmanage 済みのアラーム。捨てる */

	/*
	 * §7.3 のライフサイクル: クライアントは終了時に自分のカウンタを破棄し、
	 * それはこちらのアラーム破棄より先に起こりうる。XSync はカウンタが
	 * 消えるとアラームも道連れに破棄し、state=Destroyed の通知を 1 度だけ寄越す。
	 * その通知の counter_value は当てにならないので、target との突き合わせに
	 * かける前にここで「カウンタ無し」へ落とす。放置すると WAITING のまま
	 * 250 ms を待たされたうえ、二度と発火しないアラームを掴み続けることになる。
	 * サーバ側では既に破棄済みなので DestroyAlarm は送らない。
	 */
	if (an->state == XCB_SYNC_ALARMSTATE_DESTROYED) {
		LOG("sync: win=0x%08x のカウンタが先に破棄された。"
		    "カウンタ無しへ落とす (§7.3)", (unsigned)c->win);
		c->sync_alarm = XCB_NONE;
		sync_drop(c);
		return true;
	}

	/* ワート対策（カウンタの実在確認）と、未採番（target==0）の門前払い */
	if (c->sync_counter == XCB_NONE || c->sync_target == 0)
		return true;

	/*
	 * §7.3: **必ず target と突き合わせる**。
	 * 既に上書きされた古いリクエストに対する通知が遅れて届くことがあり
	 * （ボタン解放時の採番し直しでも普通に起きる）、これを充足と見なすと
	 * まだ描き終えていないクライアントへ次のジオメトリを送ってしまう。
	 * counter >= target を満たさない通知は捨てる。
	 */
	if (!int64_ge_u64(an->counter_value, c->sync_target))
		return true;

	if (c->sync_state != SYNC_WAITING && c->sync_state != SYNC_STALLED)
		return true;   /* 待っていない。取りこぼしでも何でもない */

	c->sync_state = SYNC_IDLE;
	/*
	 * §7.3: 「そのクライアントが後に一度でもカウンタを更新したら、
	 * 記録を解除して同期に復帰させる」。STALLED のまま遅れて充足した
	 * 場合もここを通るので、沈黙していたクライアントが息を吹き返せば
	 * 次のドラッグからは滑らかに戻る。
	 */
	c->flags &= ~(uint32_t)CF_SYNC_UNFIT;

	/*
	 * §7.3:「保留ジオメトリがあれば即座に次を送る」。
	 * 保留を持っているのは move.c であり、その吐き出し口は move_tick である
	 * （保留の有無・16 ms のレート上限・WAITING の閂はすべて move_tick 内で
	 * 判定される）。ドラッグ中でなければ move_tick は即座に戻る。
	 */
	if (move_active())
		move_tick(wm_now_ms());

	return true;
}

/*
 * WAITING のまま WM_SYNC_TIMEOUT_MS を過ぎたクライアントを STALLED へ落とす。
 *
 * move.c もドラッグ中のクライアント 1 件について同じ判定を持つが、こちらは
 * 全クライアントを見る。ドラッグ終端で送ったリクエスト（§7.3「ボタン解放時の
 * 扱い」）が返らないまま操作が終わった場合、move.c 側はもう誰も見ていないため、
 * この関数が無いと WAITING のまま取り残され、次のドラッグの出だしが
 * 「未確定 1 件」の閂で塞がる。
 */
void sync_check_timeout(uint64_t now_ms)
{
	struct client *c;

	if (!wm.have_sync)
		return;

	for (c = wm.stack_bottom; c != NULL; c = c->next) {
		if (c->sync_state != SYNC_WAITING)
			continue;
		if (c->sync_counter == XCB_NONE)   /* ワート対策 */
			continue;
		if (now_ms < c->sync_sent_ms)      /* 時刻が巻き戻ったら次回へ */
			continue;
		if (now_ms - c->sync_sent_ms < (uint64_t)WM_SYNC_TIMEOUT_MS)
			continue;

		c->sync_state = SYNC_STALLED;
		/*
		 * §7.3: 記録はセッション中ずっと有効（sticky）。
		 * 解除は「実際にカウンタが充足した」ときだけ（sync_handle_event）。
		 * ドラッグごとに解除すると、カウンタを持つが更新しないクライアントで
		 * 毎回 250 ms の待ちが復活する。
		 */
		c->flags |= CF_SYNC_UNFIT;
		LOG("sync: win=0x%08x が %u ms 応答せず。"
		    "STALLED へ落とし同期不適合として記録する (§7.3)",
		    (unsigned)c->win, (unsigned)WM_SYNC_TIMEOUT_MS);
	}
}
