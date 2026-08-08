#!/bin/sh
#
# 050-focus.sh
#
# SPEC §3.6: フォーカスの変化に応じて _NET_ACTIVE_WINDOW が
# 追従することを確認する (xdotool windowactivate による能動的な切替)。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm

start_xvfb
start_wm

xterm -display "$DISPLAY" -T w98wm-focus-test-1 >/dev/null 2>&1 &
P1=$!
_LIB_PIDS="$_LIB_PIDS $P1"
win1=$(wait_for_window --pid "$P1" 10) || fail "1つ目の xterm のウィンドウが現れませんでした"

xterm -display "$DISPLAY" -T w98wm-focus-test-2 >/dev/null 2>&1 &
P2=$!
_LIB_PIDS="$_LIB_PIDS $P2"
win2=$(wait_for_window --pid "$P2" 10) || fail "2つ目の xterm のウィンドウが現れませんでした"

win1_hex=$(to_hex "$win1")
win2_hex=$(to_hex "$win2")

active_window_hex() {
	xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null |
		awk -F'# ' '{print $2}' | awk '{print $1}'
}

xdotool windowactivate --sync "$win2" >/dev/null 2>&1 || fail "windowactivate(win2) に失敗しました"
sleep 0.3
active=$(active_window_hex)
[ "$active" = "$win2_hex" ] ||
	fail "_NET_ACTIVE_WINDOW ($active) が win2 ($win2_hex) を指していません"

xdotool windowactivate --sync "$win1" >/dev/null 2>&1 || fail "windowactivate(win1) に失敗しました"
sleep 0.3
active=$(active_window_hex)
[ "$active" = "$win1_hex" ] ||
	fail "_NET_ACTIVE_WINDOW ($active) が win1 ($win1_hex) を指していません"

echo "OK: _NET_ACTIVE_WINDOW がフォーカス変更 (win2 -> win1) に追従しました"
exit 0
