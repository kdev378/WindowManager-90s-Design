#!/bin/sh
#
# 020-map-manage.sh
#
# SPEC §3.1 / §10-1: xterm を起動すると _NET_CLIENT_LIST に現れ、
# reparent される (親がルートウィンドウでなくなる)ことを確認する。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm

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
*) fail "xterm ($winhex) が _NET_CLIENT_LIST にありません: $list" ;;
esac

root=$(get_root_window)
parent=$(get_parent_window "$win")

if [ -n "$parent" ] && [ -n "$root" ]; then
	[ "$parent" != "$root" ] || fail "xterm ($winhex) がルート直下のままです (reparent されていません)"
else
	echo "注記: xwininfo が使えないため reparent の直接確認ができません。WM_STATE の有無で代替確認します。"
	xprop -display "$DISPLAY" -id "$winhex" WM_STATE >/dev/null 2>&1 ||
		fail "WM_STATE が無く、管理されていないようです"
fi

echo "OK: xterm ($winhex) は _NET_CLIENT_LIST に現れ、reparent されています"
exit 0
