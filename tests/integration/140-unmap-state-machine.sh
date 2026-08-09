#!/bin/sh
#
# 140-unmap-state-machine.sh
#
# UnmapNotify の 4 系統を **個別に** 検証する（SPEC §3.8, ICCCM §4.1.4）。
#
# なぜ個別に要るか
# ----------------
# WM から見ると UnmapNotify の発生源は 4 つある:
#
#   (1) 自分の reparent  … client_manage が frame へ移すとき
#   (2) 最小化           … WM が client と frame を unmap するとき
#   (3) デスクトップ切替 … WM が frame を unmap するとき
#   (4) クライアント自身の UnmapWindow … これだけが Withdrawn を意味する
#
# (1)(2)(3) は無視し、(4) だけを「管理をやめる」と解釈しなければならない。
# 実装は 1 本のカウンタ (unmap_pending) でこれを捌いているので、
# **数え方を 1 つ間違えるだけで全部崩れる**。しかも壊れ方が
# 「窓が勝手に消える」「最小化から戻ってこない」という、
# 事後に原因を辿るのが最も難しい形になる。
#
# 実際にこのプロジェクトでは 2 回踏んでいる:
#   - 1 回の UnmapWindow で UnmapNotify が 2 通届くことを見落とし、
#     最小化するとウィンドウが消えた
#   - frame だけを unmap する経路でカウンタを加算してしまい、
#     次の本物の withdraw を食い潰した
#
# 実アプリでは (4) を狙って起こせないので、専用クライアントで叩く。
#
# 検証項目:
#   1. reparent 由来の Unmap でクライアントリストから消えないこと
#   2. 最小化 → WM_STATE=Iconic、リストには**残る** → 復元 → Normal
#   3. デスクトップ切替 → リストに残る → 戻すと再び可視
#   4. クライアント自身の UnmapWindow → **リストから消える** (Withdrawn)
#      → 再び MapWindow すると管理下に戻る
#   5. 上記を数回繰り返してもカウンタがずれない
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/unmapclient.c" <<'EOF'
/*
 * 自分で map/unmap できる検証クライアント。
 * 標準入力: 'u' = UnmapWindow(自分), 'm' = MapWindow, 'q' = 終了
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

int main(void)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w;
	uint32_t vals[1];
	const char *title = "w98wm-unmap-probe";
	int ch;

	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	w = xcb_generate_id(c);
	vals[0] = 0xc0c0c0;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 60, 60, 240, 160, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL, vals);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);

	xcb_map_window(c, w);
	xcb_flush(c);
	printf("0x%x\n", w);
	fflush(stdout);

	while ((ch = getchar()) != EOF) {
		if (ch == 'u') {
			xcb_unmap_window(c, w);
			xcb_flush(c);
		} else if (ch == 'm') {
			xcb_map_window(c, w);
			xcb_flush(c);
		} else if (ch == 'q') {
			break;
		}
	}
	for (;;)
		sleep(1);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/unmapclient" "$TMP/unmapclient.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

# デスクトップ切替を試すので 2 面にする
CFG="$TMP/cfg"
mkdir -p "$CFG/w98wm"
printf 'desktops=2\n' >"$CFG/w98wm/config"
XDG_CONFIG_HOME="$CFG"
export XDG_CONFIG_HOME

start_xvfb
start_wm

in_client_list() {
	xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
		sed 's/.*# *//' | tr ',' '\n' | tr -d ' ' | grep -qix "$1"
}
wm_state() {
	xprop -display "$DISPLAY" -id "$1" WM_STATE 2>/dev/null |
		awk '/window state:/ { print $3; exit }'
}
map_state() {
	xwininfo -display "$DISPLAY" -id "$1" 2>/dev/null |
		awk '/Map State:/ { print $3; exit }'
}
wait_until() {   # wait_until <秒> <コマンド...>
	__t="$1"; shift
	__i=0
	while [ $__i -lt $((__t * 5)) ]; do
		"$@" && return 0
		sleep 0.2
		__i=$((__i + 1))
	done
	return 1
}
# wait_until から呼ぶための述語 (wait_until は引数をそのまま実行するので、
# $(...) を引数に書くと評価が 1 回きりになってしまう)
is_wm_state() { [ "$(wm_state "$1")" = "$2" ]; }
is_map_state() { [ "$(map_state "$1")" = "$2" ]; }
not_in_client_list() { ! in_client_list "$1"; }

FIFO="$TMP/fifo"
mkfifo "$FIFO"
"$TMP/unmapclient" <"$FIFO" >"$TMP/win.txt" 2>"$TMP/err.txt" &
CL_PID=$!
_LIB_PIDS="$_LIB_PIDS $CL_PID"
exec 3>"$FIFO"

win=""
i=0
while [ $i -lt 60 ]; do
	win=$(head -n1 "$TMP/win.txt" 2>/dev/null)
	[ -n "$win" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$win" ] || fail "検証クライアントが窓を作れませんでした: $(cat "$TMP/err.txt")"

# ------------------------------------------------------------------
# 1. reparent 由来の Unmap で消えないこと
# ------------------------------------------------------------------
wait_until 10 in_client_list "$win" ||
	fail "(1) reparent 後にクライアントリストに現れません ($win)"
[ "$(wm_state "$win")" = "Normal" ] ||
	fail "(1) WM_STATE が Normal ではありません: $(wm_state "$win")"

# ------------------------------------------------------------------
# 2. 最小化 → Iconic、リストには残る → 復元 → Normal
# ------------------------------------------------------------------
if command -v xdotool >/dev/null 2>&1; then
	xdotool windowminimize "$win" >/dev/null 2>&1 || true
else
	skip "xdotool がありません"
fi

wait_until 10 is_wm_state "$win" Iconic ||
	fail "(2) 最小化しても WM_STATE が Iconic になりません: $(wm_state "$win")"
in_client_list "$win" ||
	fail "(2) 最小化でクライアントリストから消えました。最小化は Withdrawn ではありません"

printf 'm' >&3          # クライアント自身が map し直す = 復元要求
wait_until 10 is_wm_state "$win" Normal ||
	fail "(2) 復元しても WM_STATE が Normal に戻りません: $(wm_state "$win")"
[ "$(map_state "$win")" = "IsViewable" ] ||
	fail "(2) 復元後に可視になっていません: $(map_state "$win")"

# ------------------------------------------------------------------
# 3. デスクトップ切替でリストに残り、戻すと再び可視
# ------------------------------------------------------------------
n_desk=$(xprop -display "$DISPLAY" -root _NET_NUMBER_OF_DESKTOPS 2>/dev/null |
	sed 's/.*= *//')
if [ "${n_desk:-1}" -ge 2 ] 2>/dev/null; then
	if command -v wmctrl >/dev/null 2>&1; then
		wmctrl -s 1 >/dev/null 2>&1 || true
		sleep 1

		in_client_list "$win" ||
			fail "(3) デスクトップを切り替えたらクライアントリストから消えました"

		#
		# ★ ここは IsUnMapped ではなく **IsUnviewable** が正しい。
		#
		#   デスクトップ切替では WM は **frame だけを unmap** する
		#   (§3.8 の発生源 3)。クライアントウィンドウ自身は map された
		#   ままなので、xwininfo からは「自分は map されているが
		#   親が unmap されている」= IsUnviewable に見える。
		#
		#   これは unmap_pending の設計に直結する区別である。frame だけを
		#   unmap する経路では c->win 宛の UnmapNotify が発生しないので、
		#   ここでカウンタを加算してはならない。加算すると次の本物の
		#   withdraw を食い潰す（実際に踏んだバグ）。
		#   つまり IsUnMapped が観測されたら、それは
		#   「クライアントごと unmap してしまっている」= 設計から外れた実装。
		#
		ms=$(map_state "$win")
		[ "$ms" = "IsUnviewable" ] ||
			fail "(3) 別デスクトップの窓が IsUnviewable ではありません: $ms（frame だけを unmap する設計から外れている）"

		wmctrl -s 0 >/dev/null 2>&1 || true
		sleep 1
		wait_until 10 is_map_state "$win" IsViewable ||
			fail "(3) デスクトップを戻しても可視になりません: $(map_state "$win")"
		in_client_list "$win" ||
			fail "(3) デスクトップを戻したらクライアントリストから消えました"
	else
		echo "# wmctrl が無いのでデスクトップ切替は省略"
	fi
else
	echo "# デスクトップが 1 面しかないので切替は省略 (n=$n_desk)"
fi

# ------------------------------------------------------------------
# 4. クライアント自身の UnmapWindow だけが Withdrawn を意味する
# ------------------------------------------------------------------
printf 'u' >&3
wait_until 10 not_in_client_list "$win" ||
	fail "(4) クライアント自身が UnmapWindow してもリストに残っています。Withdrawn として扱われていません"

# 再び map すると管理下に戻る
printf 'm' >&3
wait_until 10 in_client_list "$win" ||
	fail "(4) Withdrawn のあと再び map しても管理下に戻りません"
[ "$(wm_state "$win")" = "Normal" ] ||
	fail "(4) 再管理後の WM_STATE が Normal ではありません: $(wm_state "$win")"

# ------------------------------------------------------------------
# 5. 繰り返してもカウンタがずれない
#
#    unmap_pending の加減算が 1 回でもずれていると、
#    ここで「消えるはずが残る」か「残るはずが消える」に化ける。
# ------------------------------------------------------------------
k=0
while [ $k -lt 4 ]; do
	xdotool windowminimize "$win" >/dev/null 2>&1 || true
	sleep 0.5
	in_client_list "$win" ||
		fail "(5) ${k}周目: 最小化でリストから消えました"

	printf 'm' >&3
	sleep 0.5
	wait_until 5 is_wm_state "$win" Normal ||
		fail "(5) ${k}周目: 復元できません ($(wm_state "$win"))"

	printf 'u' >&3
	wait_until 5 not_in_client_list "$win" ||
		fail "(5) ${k}周目: 自前の unmap で Withdrawn になりません"

	printf 'm' >&3
	wait_until 5 in_client_list "$win" ||
		fail "(5) ${k}周目: 再 map で管理下に戻りません"

	k=$((k + 1))
done

exec 3>&-
echo "# Unmap の 4 系統 (reparent / 最小化 / デスクトップ切替 / 自前 unmap) を個別に確認"
exit 0
