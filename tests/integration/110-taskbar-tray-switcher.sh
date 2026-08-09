#!/bin/sh
#
# 110-taskbar-tray-switcher.sh
#
# Phase 4 の面（SPEC §4.8 タスクバー / システムトレイ、§4.9 スタートメニュー、
# §6 Alt+Tab スイッチャ）の検証。
#
# 検証項目:
#   1. タスクバーのパネルウィンドウが存在し、_NET_WM_WINDOW_TYPE_DOCK である
#   2. パネルの strut が作業領域に反映されている
#      （**内蔵タスクバーは override-redirect でクライアント一覧に載らない**
#        ため、layout.c がここを取りこぼすと素通りする。実際に一度そうなった）
#   3. 最大化したウィンドウがタスクバーの上で止まる（strut が効いている証拠）
#   4. システムトレイのマネージャセレクション _NET_SYSTEM_TRAY_S<n> の所有者がいる
#   5. Ctrl+Esc でスタートメニューが開き、Esc で閉じる
#      （ポップアップは override-redirect なので、ルートの子の増減で見る）
#   6. Alt+Tab でフォーカスが別のウィンドウへ移る
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm
require_tool xwininfo

start_xvfb
start_wm

SCREEN_W=1280
SCREEN_H=1024

# ------------------------------------------------------------------
# 1. タスクバーのパネル
# ------------------------------------------------------------------
# start_wm() は _NET_SUPPORTING_WM_CHECK が立った時点で戻るが、
# taskbar_init() は wm_init() の後なので、パネルはまだ無いことがある。
panel=""
i=0
while [ $i -lt 40 ]; do
	panel=$(xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
		awk '/"w98wm taskbar"/ { print $1; exit }')
	[ -n "$panel" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$panel" ] || fail "タスクバーのパネルウィンドウが見つかりません"

xprop -display "$DISPLAY" -id "$panel" _NET_WM_WINDOW_TYPE 2>/dev/null |
	grep -q '_NET_WM_WINDOW_TYPE_DOCK' ||
	fail "パネルが _NET_WM_WINDOW_TYPE_DOCK になっていません"

# パネル自身の strut。下端に張っていること
strut=$(xprop -display "$DISPLAY" -id "$panel" _NET_WM_STRUT_PARTIAL 2>/dev/null |
	sed 's/.*= //')
[ -n "$strut" ] || fail "パネルに _NET_WM_STRUT_PARTIAL がありません"
bottom=$(echo "$strut" | cut -d, -f4 | tr -d ' ')
[ "$bottom" -gt 0 ] 2>/dev/null || fail "strut の bottom が 0 です: '$strut'"

# ------------------------------------------------------------------
# 2. 作業領域に反映されているか
# ------------------------------------------------------------------
wa=$(xprop -display "$DISPLAY" -root _NET_WORKAREA 2>/dev/null | sed 's/.*= //')
wa_h=$(echo "$wa" | cut -d, -f4 | tr -d ' ')
[ -n "$wa_h" ] || fail "_NET_WORKAREA が読めません"
expect_h=$((SCREEN_H - bottom))
[ "$wa_h" = "$expect_h" ] ||
	fail "作業領域の高さが strut を引いていません: workarea=$wa_h 期待=$expect_h (strut bottom=$bottom)"

# ------------------------------------------------------------------
# 3. 最大化がタスクバーの上で止まるか
# ------------------------------------------------------------------
xterm -display "$DISPLAY" -geometry 40x12+100+100 -T w98wm-tb-test >/dev/null 2>&1 &
XT1=$!
_LIB_PIDS="$_LIB_PIDS $XT1"
win1=$(wait_for_window --pid "$XT1" 10) || fail "xterm が現れませんでした"
sleep 0.5

xdotool windowactivate --sync "$win1" >/dev/null 2>&1 || true
# EWMH のクライアントメッセージで最大化する（キーバインドに依存しない）。
# ここで Super を押してはいけない —— 既定バインドでスタートメニューが開き、
# 後段の「Ctrl+Esc で開くか」の判定が「開いていたものが閉じた」に化ける。
wmctrl_ok=0
if command -v wmctrl >/dev/null 2>&1; then
	wmctrl -i -r "$(to_hex "$win1")" -b add,maximized_vert,maximized_horz && wmctrl_ok=1
fi
if [ "$wmctrl_ok" = 1 ]; then
	sleep 0.8
	# クライアント領域ではなく **フレーム** で見る。geom = workarea に
	# してしまうと装飾の分だけフレームが外へはみ出し、
	# キャプションが画面外・下端がタスクバーの上、という状態になる。
	frame=$(get_parent_window "$win1")
	[ -n "$frame" ] || frame=$(to_hex "$win1")
	geo=$(xwininfo -display "$DISPLAY" -id "$frame" 2>/dev/null)
	gy=$(echo "$geo" | awk '/Absolute upper-left Y/ { print $4 }')
	gx=$(echo "$geo" | awk '/Absolute upper-left X/ { print $4 }')
	gh=$(echo "$geo" | awk '/^  Height:/ { print $2 }')
	bot=$((gy + gh))
	[ "$gy" -ge 0 ] ||
		fail "最大化した窓のキャプションが画面外に出ています: フレーム上端=$gy"
	[ "$gx" -ge 0 ] ||
		fail "最大化した窓が画面左外に出ています: フレーム左端=$gx"
	[ "$bot" -le "$expect_h" ] ||
		fail "最大化した窓がタスクバーに潜り込んでいます: 下端=$bot 作業領域=$expect_h"
else
	echo "# wmctrl が無いので最大化の確認は省略"
fi

# ------------------------------------------------------------------
# 4. システムトレイのセレクション
# ------------------------------------------------------------------
# セレクション所有者は xprop では直接引けないので、所有者ウィンドウにだけ
# 載せているプロパティ (_NET_SYSTEM_TRAY_ORIENTATION / _VISUAL) の有無で見る。
# これらは tray_init() がセレクション取得と同時に設定するため、
# 「取れなかったので何もしなかった」場合には存在しない。
tray=""
i=0
while [ $i -lt 30 ]; do
	tray=$(xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
		awk '/"w98wm tray"/ { print $1; exit }')
	[ -n "$tray" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$tray" ] || fail "システムトレイのウィンドウが見つかりません"

xprop -display "$DISPLAY" -id "$tray" _NET_SYSTEM_TRAY_ORIENTATION 2>/dev/null |
	grep -q 'CARDINAL' ||
	fail "_NET_SYSTEM_TRAY_ORIENTATION がありません (セレクションを取れていない)"
xprop -display "$DISPLAY" -id "$tray" _NET_SYSTEM_TRAY_VISUAL 2>/dev/null |
	grep -qi 'visual' ||
	fail "_NET_SYSTEM_TRAY_VISUAL がありません (§4.8 の ARGB 対策が効いていない)"

# ------------------------------------------------------------------
# 5. スタートメニュー (Ctrl+Esc)
# ------------------------------------------------------------------
count_children() {
	xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
		awk '/children:/ { print $1; exit }'
}
before=$(count_children)
xdotool key --clearmodifiers ctrl+Escape
sleep 0.8
during=$(count_children)
[ "$during" -gt "$before" ] ||
	fail "Ctrl+Esc でスタートメニューが開きませんでした ($before -> $during)"

xdotool key --clearmodifiers Escape
sleep 0.8
after=$(count_children)
[ "$after" = "$before" ] ||
	fail "Esc でスタートメニューが閉じませんでした ($before -> $during -> $after)"

# ------------------------------------------------------------------
# 6. Alt+Tab
# ------------------------------------------------------------------
xterm -display "$DISPLAY" -geometry 40x12+300+300 -T w98wm-tb-test2 >/dev/null 2>&1 &
XT2=$!
_LIB_PIDS="$_LIB_PIDS $XT2"
win2=$(wait_for_window --pid "$XT2" 10) || fail "2 つめの xterm が現れませんでした"
sleep 0.8

xdotool windowactivate --sync "$win2" >/dev/null 2>&1 || true
sleep 0.5
active_before=$(xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null |
	sed 's/.*# //')

xdotool keydown alt
xdotool key Tab
sleep 0.6
xdotool keyup alt
sleep 0.8

active_after=$(xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null |
	sed 's/.*# //')
[ "$active_before" != "$active_after" ] ||
	fail "Alt+Tab でフォーカスが移りませんでした ($active_before のまま)"

echo "# タスクバー・トレイ・スタートメニュー・Alt+Tab すべて確認"
exit 0
