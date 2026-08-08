#!/bin/sh
#
# 030-close.sh
#
# SPEC §5.2 (_NET_CLOSE_WINDOW) / ICCCM WM_DELETE_WINDOW:
# wmctrl -c でウィンドウを閉じると _NET_CLIENT_LIST から消えることを確認する。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm
require_tool wmctrl

start_xvfb
start_wm

xterm -display "$DISPLAY" >/dev/null 2>&1 &
XTERM_PID=$!
_LIB_PIDS="$_LIB_PIDS $XTERM_PID"

win=$(wait_for_window --pid "$XTERM_PID" 10) || fail "xterm のウィンドウが現れませんでした"
winhex=$(to_hex "$win")

list=$(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null) || fail "_NET_CLIENT_LIST が読めません"
case "$list" in
*"$winhex"*) : ;;
*) fail "xterm ($winhex) が事前に _NET_CLIENT_LIST に見つかりません: $list" ;;
esac

DISPLAY="$DISPLAY" wmctrl -i -c "$winhex" || fail "wmctrl -c ($winhex) の実行に失敗しました"

closed=0
i=0
while [ $i -lt 25 ]; do
	list=$(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null) || list=""
	case "$list" in
	*"$winhex"*) : ;;
	*)
		closed=1
		break
		;;
	esac
	sleep 0.2
	i=$((i + 1))
done

[ "$closed" -eq 1 ] || fail "wmctrl -c の後も $winhex が _NET_CLIENT_LIST に残っています"

echo "OK: wmctrl -c ($winhex) の後、_NET_CLIENT_LIST から正しく消えました"
exit 0
