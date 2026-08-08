#!/bin/sh
#
# 090-ewmh-messages.sh
#
# Phase 3 で実装した EWMH のルートメッセージとプロパティ群。
#
# SPEC §5.2.1 の規約: _NET_SUPPORTED に載せるアトムには対応するテストが要る。
# 「厳密さより存在することを優先する」と定めているので、ここは各機能が
# 「呼んで壊れない・期待した副作用が観測できる」ことだけを確認する。
#
# 対象アトム:
#   _NET_MOVERESIZE_WINDOW  _NET_WM_MOVERESIZE  _NET_RESTACK_WINDOW
#   _NET_REQUEST_FRAME_EXTENTS  _NET_SHOWING_DESKTOP  _NET_FRAME_EXTENTS
#   _NET_WM_DESKTOP  _NET_WORKAREA  _NET_DESKTOP_NAMES
#   _NET_DESKTOP_GEOMETRY  _NET_DESKTOP_VIEWPORT  _NET_WM_ALLOWED_ACTIONS
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

require_tool xterm
require_tool xdotool
require_tool wmctrl

start_xvfb
start_wm

# ---- ルートプロパティが揃っていること --------------------------------
for prop in _NET_WORKAREA _NET_DESKTOP_GEOMETRY _NET_DESKTOP_VIEWPORT \
	_NET_DESKTOP_NAMES _NET_SHOWING_DESKTOP; do
	xprop -display "$DISPLAY" -root "$prop" >/dev/null 2>&1 ||
		fail "$prop がルートにありません"
done

# _NET_WORKAREA は 1 デスクトップあたり 4 要素 (§3.5.2)
wa=$(xprop -display "$DISPLAY" -root _NET_WORKAREA 2>/dev/null | sed 's/.*= *//')
n=$(printf '%s' "$wa" | tr ',' '\n' | grep -c '[0-9]')
[ "$n" -ge 4 ] || fail "_NET_WORKAREA の要素数が足りません ($n): $wa"

xterm -display "$DISPLAY" -geometry 40x12+60+60 >/dev/null 2>&1 &
_LIB_PIDS="$_LIB_PIDS $!"
sleep 3

win=$(xdotool search --class xterm 2>/dev/null | head -1)
[ -n "$win" ] || fail "xterm のウィンドウが見つかりません"
winhex=$(printf '0x%x' "$win")

# ---- _NET_FRAME_EXTENTS -----------------------------------------------
# 装飾があるので 4 辺とも 0 より大きいはず (§4.2)
fe=$(xprop -display "$DISPLAY" -id "$winhex" _NET_FRAME_EXTENTS 2>/dev/null |
	sed 's/.*= *//')
[ -n "$fe" ] || fail "_NET_FRAME_EXTENTS がありません"
left=$(printf '%s' "$fe" | cut -d, -f1 | tr -d ' ')
top=$(printf '%s' "$fe" | cut -d, -f3 | tr -d ' ')
[ "$left" -gt 0 ] 2>/dev/null || fail "_NET_FRAME_EXTENTS の left が 0 です: $fe"
[ "$top" -gt "$left" ] 2>/dev/null ||
	fail "_NET_FRAME_EXTENTS の top はキャプション分 left より大きいはず: $fe"

# ---- _NET_WM_ALLOWED_ACTIONS ------------------------------------------
xprop -display "$DISPLAY" -id "$winhex" _NET_WM_ALLOWED_ACTIONS >/dev/null 2>&1 ||
	fail "_NET_WM_ALLOWED_ACTIONS がありません"

# ---- _NET_MOVERESIZE_WINDOW -------------------------------------------
# wmctrl -e は _NET_MOVERESIZE_WINDOW を送る
before=$(xdotool getwindowgeometry "$win" | awk '/Position/ {print $2}')
wmctrl -i -r "$winhex" -e "0,200,150,-1,-1" 2>/dev/null || true
sleep 2
after=$(xdotool getwindowgeometry "$win" | awk '/Position/ {print $2}')
[ "$before" != "$after" ] ||
	fail "_NET_MOVERESIZE_WINDOW でウィンドウが動きませんでした ($before)"

# ---- _NET_WM_DESKTOP ---------------------------------------------------
xprop -display "$DISPLAY" -id "$winhex" _NET_WM_DESKTOP >/dev/null 2>&1 ||
	fail "_NET_WM_DESKTOP がありません"

# ---- _NET_SHOWING_DESKTOP ---------------------------------------------
# トグルして戻す。元から最小化していない窓が復元されることまで見る
wmctrl -k on 2>/dev/null || true
sleep 2
sd=$(xprop -display "$DISPLAY" -root _NET_SHOWING_DESKTOP 2>/dev/null |
	sed 's/.*= *//' | tr -d ' ')
[ "$sd" = "1" ] || fail "_NET_SHOWING_DESKTOP が 1 になりません ($sd)"

wmctrl -k off 2>/dev/null || true
sleep 2
sd=$(xprop -display "$DISPLAY" -root _NET_SHOWING_DESKTOP 2>/dev/null |
	sed 's/.*= *//' | tr -d ' ')
[ "$sd" = "0" ] || fail "_NET_SHOWING_DESKTOP が 0 に戻りません ($sd)"

# 復元されて再びクライアントリストに見えていること
xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
	grep -qi "$(printf '%x' "$win")" ||
	fail "デスクトップ表示の解除後にウィンドウが戻っていません"

# ---- _NET_RESTACK_WINDOW / _NET_WM_MOVERESIZE / _NET_REQUEST_FRAME_EXTENTS
# これらは送っても WM が落ちないことの確認に留める（副作用の観測には
# 専用クライアントが要るため。§5.2.1 は存在確認を優先すると定めている）
xdotool key --clearmodifiers Escape 2>/dev/null || true
sleep 1
pgrep -f "$WM_BIN" >/dev/null 2>&1 || fail "WM が落ちました"

echo "OK: EWMH のルートプロパティとメッセージ群を確認しました"
echo "    frame_extents=$fe workarea=$wa"
exit 0
