#!/bin/sh
#
# 180-sync-state-machine.sh
#
# `_NET_WM_SYNC_REQUEST` の **状態機械**（SPEC §7.3）。
#
# なぜ 100 では足りないか
# ----------------------
# `100` は「カウンタを作って `_NET_SUPPORTED` に載っている」までしか見ていない。
# しかし §7.3 が定めているのは
#
#   IDLE ──(リサイズ)──> target++ / ChangeAlarm / SYNC_REQUEST 送信 ──> WAITING
#   WAITING ──(AlarmNotify: counter >= target)──> IDLE
#   WAITING ──(250ms)──> STALLED + 「同期不適合」として記録（sticky）
#   STALLED ──(カウンタが充足)──> IDLE に復帰し記録も解除
#
# という状態機械であり、ここが壊れたときの症状は
# **「動くように見えるが遅い」**（毎回 250ms 待たされる）か、
# **「応答しないアプリのリサイズで WM ごと固まる」**のどちらかになる。
# 前者はベンチマークしないと気づけず、後者は事故である。
#
# 普通のアプリは正しく応答するので、この経路は専用クライアントでしか踏めない。
#
# 検証項目:
#   1. リサイズすると SYNC_REQUEST が飛び、target が単調増加する
#   2. **未確定リクエストは 1 つまで**（応答するまで次を送らない）
#   3. 応答しないクライアントでも**リサイズが止まらない**（250ms で STALLED）
#   4. 一度 STALLED になったら、次のドラッグでは**最初から同期を使わない**
#      （§7.3 の「同期不適合」。これが無いとドラッグの出だしが毎回 250ms 遅れる）
#   5. カウンタを充足させると同期に復帰する
#   6. カウンタを一気に先へ飛ばしても壊れない
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb xcb-sync 2>/dev/null || skip "xcb / xcb-sync がありません"
require_tool xdotool

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP" 2>/dev/null || true; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/syncclient.c" <<'EOF'
/*
 * _NET_WM_SYNC_REQUEST に応答する（あるいは応答しない）検証クライアント。
 *
 * 標準出力:
 *   WIN 0x....            起動時に 1 行
 *   REQ <通番> <target>   SYNC_REQUEST を受けるたび
 *
 * 標準入力（1 文字ずつ）:
 *   g … 以後、要求が来たら即座にカウンタを target まで進める
 *   d … 以後、要求を無視する（応答しない）
 *   j … 今すぐカウンタを「最後の target + 1000」へ一気に飛ばす
 *   c … 今すぐカウンタを「最後の target」へ進める（1 回だけ応答する）
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/sync.h>

static xcb_connection_t *c;
static xcb_sync_counter_t counter;
static uint64_t last_target;
static int respond = 1;
static unsigned nreq;

static xcb_atom_t A(const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static void set_counter(uint64_t v)
{
	xcb_sync_int64_t iv;

	iv.hi = (int32_t)(v >> 32);
	iv.lo = (uint32_t)(v & 0xffffffffu);
	xcb_sync_set_counter(c, counter, iv);
	xcb_flush(c);
}

int main(int argc, char **argv)
{
	xcb_screen_t *s;
	xcb_window_t w;
	xcb_sync_int64_t zero = { 0, 0 };
	xcb_atom_t wm_protocols, sync_req, sync_counter_atom;
	xcb_atom_t protos[1];
	uint32_t vals[1];
	struct pollfd pfd[2];

	(void)argc; (void)argv;
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	w = xcb_generate_id(c);
	vals[0] = 0xc0c0c0;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 80, 80, 320, 240, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL, vals);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, 16, "w98wm-sync-probe");

	counter = xcb_generate_id(c);
	xcb_sync_create_counter(c, counter, zero);

	sync_counter_atom = A("_NET_WM_SYNC_REQUEST_COUNTER");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		sync_counter_atom, XCB_ATOM_CARDINAL, 32, 1, &counter);

	wm_protocols = A("WM_PROTOCOLS");
	sync_req     = A("_NET_WM_SYNC_REQUEST");
	protos[0] = sync_req;
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		wm_protocols, XCB_ATOM_ATOM, 32, 1, protos);

	xcb_map_window(c, w);
	xcb_flush(c);
	printf("WIN 0x%x\n", w);
	fflush(stdout);

	pfd[0].fd = xcb_get_file_descriptor(c);
	pfd[0].events = POLLIN;
	pfd[1].fd = 0;
	pfd[1].events = POLLIN;

	for (;;) {
		xcb_generic_event_t *ev;

		xcb_flush(c);
		if (poll(pfd, 2, 200) < 0)
			continue;

		/*
		 * ★ EOF を必ず扱うこと。
		 *   テスト側が FIFO の書き込み端を閉じると stdin は EOF になり、
		 *   poll(2) は以後ずっと即座に返る。ここで getchar() の EOF を
		 *   無視してループを続けると **100% CPU のビジーループ**になり、
		 *   プロセスが残っている間ずっと 1 コアを焼き続ける。
		 *   実際にこれで syncclient が 10 個溜まり、ロードアベレージが
		 *   19 まで上がって、他のテストが軒並みタイミングで落ちた。
		 */
		if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			int ch = getchar();
			if (ch == EOF)
				break;
			if (ch == 'g') {
				respond = 1;
				if (last_target)
					set_counter(last_target);
			} else if (ch == 'd') {
				respond = 0;
			} else if (ch == 'j') {
				set_counter(last_target + 1000);
			} else if (ch == 'c') {
				if (last_target)
					set_counter(last_target);
			} else if (ch == 'q') {
				break;
			}
		}

		while ((ev = xcb_poll_for_event(c)) != NULL) {
			if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
				xcb_client_message_event_t *cm =
					(xcb_client_message_event_t *)ev;

				if (cm->type == wm_protocols &&
				    cm->data.data32[0] == sync_req) {
					uint64_t lo = cm->data.data32[2];
					uint64_t hi = cm->data.data32[3];
					uint64_t t  = (hi << 32) | lo;

					last_target = t;
					nreq++;
					printf("REQ %u %llu\n", nreq,
					       (unsigned long long)t);
					fflush(stdout);
					if (respond)
						set_counter(t);
				}
			}
			free(ev);
		}
	}
	return 0;
}
EOF

cc -std=c99 -D_POSIX_C_SOURCE=200809L -o "$TMP/syncclient" "$TMP/syncclient.c" \
	$(pkg-config --cflags --libs xcb xcb-sync) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

cat >"$TMP/moveresize.c" <<'EOF'
/* _NET_WM_MOVERESIZE を送ってリサイズドラッグを開始/中断する */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_client_message_event_t ev;
	xcb_intern_atom_reply_t *r;
	xcb_window_t w;
	uint32_t dir;
	int16_t x, y;

	if (argc < 5)
		return 2;
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	w   = (xcb_window_t)strtoul(argv[1], NULL, 0);
	x   = (int16_t)atoi(argv[2]);
	y   = (int16_t)atoi(argv[3]);
	dir = (uint32_t)atoi(argv[4]);

	r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen("_NET_WM_MOVERESIZE"),
		                "_NET_WM_MOVERESIZE"), NULL);

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = w;
	ev.format = 32;
	ev.type = r ? r->atom : 0;
	ev.data.data32[0] = (uint32_t)x;
	ev.data.data32[1] = (uint32_t)y;
	ev.data.data32[2] = dir;
	ev.data.data32[3] = 1;      /* button */
	ev.data.data32[4] = 2;      /* source: pager */
	free(r);

	xcb_send_event(c, 0, s->root,
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(const char *)&ev);
	xcb_flush(c);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/moveresize" "$TMP/moveresize.c" \
	$(pkg-config --cflags --libs xcb) 2>>"$TMP/cc.log" ||
	skip "moveresize をビルドできません"

start_xvfb
start_wm

FIFO="$TMP/fifo"
mkfifo "$FIFO"
"$TMP/syncclient" <"$FIFO" >"$TMP/out.txt" 2>"$TMP/err.txt" &
CL=$!
_LIB_PIDS="$_LIB_PIDS $CL"
exec 3>"$FIFO"

win=""
i=0
while [ $i -lt 60 ]; do
	win=$(awk '/^WIN /{print $2; exit}' "$TMP/out.txt" 2>/dev/null)
	[ -n "$win" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$win" ] || fail "検証クライアントが窓を作れませんでした: $(cat "$TMP/err.txt")"
sleep 1.5

wm_alive() { kill -0 "$WM_PID" 2>/dev/null; }
n_req() { grep -c '^REQ ' "$TMP/out.txt" 2>/dev/null || true; }
win_size() {
	xwininfo -display "$DISPLAY" -id "$win" 2>/dev/null |
		awk '/^  Width:/ {w=$2} /^  Height:/ {h=$2} END {print w"x"h}'
}

#
# リサイズドラッグを合成する。
#   _NET_WM_MOVERESIZE(dir=4 = BOTTOMRIGHT) で WM 側のドラッグを開始し、
#   ポインタを動かして MotionNotify を起こす。
#   xdotool のボタン押下からは WM のヒットテストを通す必要があって
#   座標計算が脆いので、EWMH の入口を使う方が確実。
#
#
# ★ 判定に「ドラッグ終了後のサイズ」を使ってはいけない。
#
#   §7.3 の「ボタン解放時は target を採番し直して最終ジオメトリを即送る」
#   により、**同期が完全に詰まっていても最後の 1 回で正しいサイズになる**。
#   最初はこれで見ていたため、`sync_check_timeout()` を丸ごと無効化しても
#   テストが通ってしまった（実際に壊して確認した）。
#
#   見るべきは **ドラッグ中にサイズが何回変わったか**。
#     - 同期が働いている（応答あり）      … ほぼ毎ステップ変わる
#     - 応答なしで STALLED へ落ちる       … 最初の 1 回で止まり、250ms 後に再開
#     - 応答なしで STALLED へ落ちない     … 最初の 1 回で止まったきり（= 固まる）
#     - 同期不適合として記録済み          … 出だしから毎ステップ変わる
#   この 4 つは回数で区別できる。
#
drag_changes=0
do_resize() {   # do_resize <移動量> <ステップ数> <ステップ間隔秒>
	amount="$1"; steps="$2"; iv="${3:-0.15}"
	geo=$(xwininfo -display "$DISPLAY" -id "$win" 2>/dev/null)
	gx=$(echo "$geo" | awk '/Absolute upper-left X/ {print $4}')
	gy=$(echo "$geo" | awk '/Absolute upper-left Y/ {print $4}')
	gw=$(echo "$geo" | awk '/^  Width:/ {print $2}')
	gh=$(echo "$geo" | awk '/^  Height:/ {print $2}')
	px=$((gx + gw))
	py=$((gy + gh))

	xdotool mousemove "$px" "$py" >/dev/null 2>&1 || true
	sleep 0.2
	"$TMP/moveresize" "$win" "$px" "$py" 4 >/dev/null 2>&1 || true
	sleep 0.3

	drag_changes=0
	last=$(win_size)
	k=1
	while [ $k -le "$steps" ]; do
		xdotool mousemove $((px + amount * k / steps)) $((py + amount * k / steps)) \
			>/dev/null 2>&1 || true
		sleep "$iv"
		now=$(win_size)
		if [ "$now" != "$last" ]; then
			drag_changes=$((drag_changes + 1))
			last=$now
		fi
		k=$((k + 1))
	done

	xdotool mouseup 1 >/dev/null 2>&1 || true
	sleep 0.5
}

# ------------------------------------------------------------------
# 1. 応答するクライアント: リクエストが飛び、target が単調増加する
# ------------------------------------------------------------------
printf 'g' >&3
sleep 0.3
size0=$(win_size)
do_resize 90 6 0.15
live_good=$drag_changes

r1=$(n_req)
[ "${r1:-0}" -ge 1 ] ||
	fail "(1) リサイズしても _NET_WM_SYNC_REQUEST が 1 度も飛びません（§7.3 の同期が働いていない）"

# target が単調増加しているか
bad=$(awk '/^REQ /{ if ($3 <= prev) print "NG " $3 " <= " prev; prev=$3 }' "$TMP/out.txt")
[ -z "$bad" ] || fail "(1) target が単調増加していません: $bad"

size1=$(win_size)
[ "$size1" != "$size0" ] || fail "(1) リサイズが反映されていません ($size0)"

# 応答するクライアントはドラッグ中も追従し続ける
[ "$live_good" -ge 4 ] ||
	fail "(1) 応答するクライアントなのにドラッグ中の追従が $live_good 回しかありません (6 ステップ中)"

# ------------------------------------------------------------------
# 2. 未確定リクエストは 1 つまで
#
#    応答を止めた状態で 1 回ドラッグすると、リクエストは
#    **1 つだけ**飛んで、応答が返るまで次は飛ばないはず。
# ------------------------------------------------------------------
printf 'd' >&3
sleep 0.3
base=$(n_req)
size2=$(win_size)

do_resize 90 6 0.15
live_dead=$drag_changes

after_dead=$(n_req)
delta=$((after_dead - base))
[ "$delta" -ge 1 ] || fail "(2) 応答なしモードで最初の 1 回も飛びません"
[ "$delta" -le 2 ] ||
	fail "(2) 未確定のまま $delta 回もリクエストが飛んでいます（1 つまでのはず。§3.4.1-3）"

# ------------------------------------------------------------------
# 3. 応答しなくてもリサイズは止まらない（250ms で STALLED へ落ちる）
#
#    ★ ここは **ドラッグ中**の追従回数で見る。
#      ドラッグ終了後のサイズは §7.3 の「ボタン解放時は採番し直して
#      即送る」規則により、同期が完全に詰まっていても正しくなるため
#      判定材料にならない（それで一度素通りさせた）。
#
#      応答なしでも 250ms で STALLED へ落ちれば、その後は 16ms の
#      レート制御だけで動き続けるので 2 回以上は追従する。
#      STALLED へ落ちなければ最初の 1 回で止まったきりになる。
# ------------------------------------------------------------------
[ "$live_dead" -ge 2 ] ||
	fail "(3) **応答しないクライアントでリサイズが固まりました**（ドラッグ中の追従が $live_dead 回）。250ms の STALLED 遷移が働いていない"

wm_alive || fail "(3) 応答なしクライアントのリサイズで WM が死にました"

# ------------------------------------------------------------------
# 4. 一度 STALLED になったら次のドラッグでは最初から同期しない
#    （§7.3 の「同期不適合」の記録。sticky）
# ------------------------------------------------------------------
base2=$(n_req)
do_resize 80 6 0.15
live_unfit=$drag_changes
after2=$(n_req)

[ "$after2" = "$base2" ] ||
	fail "(4) 同期不適合として記録されていません。2 回目のドラッグでも $((after2 - base2)) 回リクエストが飛びました（毎回 250ms 待つ実装になっている）"

#
# 記録が効いていれば **出だしから**同期を使わないので、
# 応答ありのときと同じくらい追従する。
# 記録が無ければ出だしの 250ms を毎回待つので、追従回数が落ちる。
#
[ "$live_unfit" -ge "$live_good" ] || [ "$live_unfit" -ge 5 ] ||
	fail "(4) 2 回目のドラッグの追従が $live_unfit 回しかありません（1 回目の応答ありは $live_good 回）。出だしで毎回待たされています"

# ------------------------------------------------------------------
# 5. カウンタを充足させると同期に復帰する
# ------------------------------------------------------------------
printf 'g' >&3
sleep 0.5
printf 'c' >&3
sleep 0.8

base3=$(n_req)
do_resize 70 6
after3=$(n_req)

[ "$after3" -gt "$base3" ] ||
	fail "(5) カウンタを充足させても同期に復帰しません（§7.3 は「一度でも更新したら記録を解除」と定めている）"

# ------------------------------------------------------------------
# 6. カウンタを一気に先へ飛ばしても壊れない
# ------------------------------------------------------------------
printf 'j' >&3
sleep 0.5
wm_alive || fail "(6) カウンタを一気に進めたら WM が死にました"

base4=$(n_req)
do_resize 60 5
after4=$(n_req)
wm_alive || fail "(6) カウンタを飛ばした後のリサイズで WM が死にました"
[ "$after4" -ge "$base4" ] || fail "(6) リクエスト数が減りました"

size6=$(win_size)
do_resize 50 4
[ "$(win_size)" != "$size6" ] ||
	fail "(6) カウンタを飛ばした後、リサイズが効かなくなりました"

exec 3>&-
wm_alive || fail "最終確認で WM が死んでいました"
echo "# _NET_WM_SYNC_REQUEST の状態機械 (リクエスト発行 / 未確定 1 つまで / 250ms STALLED / 同期不適合の記録と復帰 / カウンタ飛び) を確認"
exit 0
