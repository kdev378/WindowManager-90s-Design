#!/bin/sh
#
# 010-ewmh-supported.sh
#
# SPEC §5.2.1 / §10-2: ルートウィンドウに _NET_SUPPORTED があり、
# 必須アトム群を含み、_NET_SUPPORTING_WM_CHECK が指すチェックウィンドウが
# 自分自身を指し返していることを確認する。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

start_xvfb
start_wm

REQUIRED_ATOMS="
_NET_SUPPORTING_WM_CHECK
_NET_CLIENT_LIST
_NET_ACTIVE_WINDOW
_NET_WM_NAME
_NET_WM_STATE
_NET_WM_WINDOW_TYPE
_NET_WM_WINDOW_TYPE_DESKTOP
_NET_WM_WINDOW_TYPE_DOCK
_NET_WM_WINDOW_TYPE_TOOLBAR
_NET_WM_WINDOW_TYPE_MENU
_NET_WM_WINDOW_TYPE_UTILITY
_NET_WM_WINDOW_TYPE_SPLASH
_NET_WM_WINDOW_TYPE_DIALOG
_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
_NET_WM_WINDOW_TYPE_POPUP_MENU
_NET_WM_WINDOW_TYPE_TOOLTIP
_NET_WM_WINDOW_TYPE_NOTIFICATION
_NET_WM_WINDOW_TYPE_COMBO
_NET_WM_WINDOW_TYPE_DND
_NET_WM_WINDOW_TYPE_NORMAL
"

supported=$(xprop -display "$DISPLAY" -root _NET_SUPPORTED 2>/dev/null) ||
	fail "_NET_SUPPORTED がルートウィンドウにありません"

for atom in $REQUIRED_ATOMS; do
	case "$supported" in
	*"$atom"*) : ;;
	*) fail "_NET_SUPPORTED に $atom がありません" ;;
	esac
done

checkprop=$(xprop -display "$DISPLAY" -root _NET_SUPPORTING_WM_CHECK 2>/dev/null) ||
	fail "_NET_SUPPORTING_WM_CHECK がルートウィンドウにありません"

checkwin_hex=$(printf '%s\n' "$checkprop" | awk -F'# ' '{print $2}' | awk '{print $1}')
[ -n "$checkwin_hex" ] || fail "_NET_SUPPORTING_WM_CHECK の値を解釈できません: $checkprop"

self_check=$(xprop -display "$DISPLAY" -id "$checkwin_hex" _NET_SUPPORTING_WM_CHECK 2>/dev/null) ||
	fail "チェックウィンドウ ($checkwin_hex) に _NET_SUPPORTING_WM_CHECK がありません"

case "$self_check" in
*"$checkwin_hex"*) : ;;
*) fail "チェックウィンドウが自分自身を指し返していません ($self_check)" ;;
esac

echo "OK: _NET_SUPPORTED の必須アトムとチェックウィンドウの自己参照を確認しました"
exit 0
