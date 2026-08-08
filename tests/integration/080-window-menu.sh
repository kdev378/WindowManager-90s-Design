#!/bin/sh
#
# 080-window-menu.sh
#
# SPEC §4.6: ウィンドウメニューの開閉と項目選択を検証する。
#
#   - Alt+Space でウィンドウメニューが開く
#   - タイトルアイコンの左クリックでもウィンドウメニューが開く
#   - 開いたメニューは override-redirect な新規ウィンドウとして現れる
#     (通常のクライアントとして管理されない、というポップアップの性質)
#   - Esc でメニューが閉じる (クライアント自体は閉じない)
#   - 「閉じる(C)」を選ぶとクライアントが閉じる
#     (項目は SPEC §4.6 の順序で最後: 元のサイズに戻す/移動/サイズ変更/
#      最小化/最大化/閉じる。メニュー内部のレイアウト詳細に依存しないよう、
#      実際に開いたメニューウィンドウの実測ジオメトリから
#      「一番下の項目」の座標を逆算してクリックする)
#
# src/menu.c が未実装の場合は skip (exit 77) する。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm
require_tool xwininfo

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [ ! -f "$ROOT_DIR/src/menu.c" ]; then
	skip "src/menu.c がまだ実装されていません (Phase 2 進行中の可能性があります)"
fi

# SPEC §4.2 のメトリクス (070-decoration.sh と同じ前提)
BORDER=4
CAPTION_H=18
ICON_SIZE=16
ICON_OFFSET=2 # キャプション左端からアイコンまでの距離
MENU_ITEM_H=18

start_xvfb
start_wm

xterm -display "$DISPLAY" -geometry 80x24+80+80 -T w98wm-menu-test >/dev/null 2>&1 &
XTERM_PID=$!
_LIB_PIDS="$_LIB_PIDS $XTERM_PID"

win=$(wait_for_window --pid "$XTERM_PID" 10) || fail "xterm のウィンドウが現れませんでした"
winhex=$(to_hex "$win")

xdotool windowactivate --sync "$win" >/dev/null 2>&1 || true
sleep 0.3

# --- ヘルパ ---

# lib.sh の get_parent_window() は "-children" を付けずに xwininfo を呼ぶため、
# 手元の xwininfo (x11-apps 由来) では "Parent window id:" 行自体が出ず、
# 常に空を返してしまう。frame の実体そのものが要るので自前に取得する。
find_frame_window() {
	xwininfo -display "$DISPLAY" -id "$1" -children 2>/dev/null |
		awk '/Parent window id:/ { print $4; exit }'
}

# xwininfo の "ラベル: 値" 形式の1行から値を取り出す (空白除去)
win_field() {
	id="$1"
	label="$2"
	xwininfo -display "$DISPLAY" -id "$id" -all </dev/null 2>/dev/null |
		sed -n "s/^[[:space:]]*${label}:[[:space:]]*//p" | head -n1 | tr -d '[:space:]'
}

# root の直下の子ウィンドウID一覧 (16進, 1行1個)
list_root_children() {
	xwininfo -display "$DISPLAY" -root -children </dev/null 2>/dev/null |
		grep -E '^ *0x[0-9A-Fa-f]+' | awk '{print $1}'
}

# baseline (ファイル) に無い、新しく現れた root の子を最大 timeout 秒待って1つ返す
wait_new_child() {
	baseline_file="$1"
	timeout="${2:-5}"
	i=0
	max=$((timeout * 5))
	while [ $i -lt $max ]; do
		list_root_children >"$WORKDIR/cur_children"
		new=$(grep -vFxf "$baseline_file" "$WORKDIR/cur_children" 2>/dev/null || true)
		if [ -n "$new" ]; then
			echo "$new" | head -n1
			return 0
		fi
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

# ウィンドウが消える (destroy されるか IsViewable でなくなる) のを待つ
wait_gone_or_unmapped() {
	id="$1"
	timeout="${2:-5}"
	i=0
	max=$((timeout * 5))
	while [ $i -lt $max ]; do
		state=$(win_field "$id" "Map State")
		if [ -z "$state" ] || [ "$state" != "IsViewable" ]; then
			return 0
		fi
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

client_in_list() {
	list=$(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null) || return 1
	case "$list" in
	*"$winhex"*) return 0 ;;
	*) return 1 ;;
	esac
}

frame_hex=$(find_frame_window "$winhex")
[ -n "$frame_hex" ] || skip "xwininfo で frame ウィンドウを特定できません"

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"; _lib_cleanup' EXIT INT TERM

FAILED=0

# ==================================================================
# (A) Alt+Space でメニューを開き、override-redirect であることを確認し、
#     Esc で閉じる (クライアントは生き残る) ことを確認する。
# ==================================================================
echo "--- (A) Alt+Space でウィンドウメニューを開く ---"
list_root_children >"$WORKDIR/baseline_a"

xdotool key --clearmodifiers alt+space >/dev/null 2>&1 || fail "xdotool key alt+space の送出に失敗しました"

menu1=$(wait_new_child "$WORKDIR/baseline_a" 5) || fail "Alt+Space の後、新しいウィンドウ (メニュー) が現れませんでした"
echo "OK: Alt+Space でウィンドウ $menu1 が現れました"

ov=$(win_field "$menu1" "Override Redirect State")
ov_lc=$(printf '%s' "$ov" | tr 'A-Z' 'a-z')
if [ "$ov_lc" != "yes" ]; then
	echo "FAIL: メニューウィンドウ ($menu1) の Override Redirect State が yes ではありません (実際: '$ov')" >&2
	FAILED=1
else
	echo "OK: メニューウィンドウ ($menu1) は override-redirect です"
fi

xdotool key --clearmodifiers Escape >/dev/null 2>&1 || fail "xdotool key Escape の送出に失敗しました"

if wait_gone_or_unmapped "$menu1" 5; then
	echo "OK: Esc でメニューウィンドウ ($menu1) が閉じました"
else
	echo "FAIL: Esc の後もメニューウィンドウ ($menu1) が IsViewable のままです" >&2
	FAILED=1
fi

if client_in_list; then
	echo "OK: Esc の後も xterm ($winhex) は _NET_CLIENT_LIST に残っています (クライアントは閉じていない)"
else
	echo "FAIL: Esc でメニューを閉じただけのはずが、xterm ($winhex) まで閉じてしまいました" >&2
	FAILED=1
fi

# ==================================================================
# (B) タイトルアイコンの左クリックでメニューを開き、
#     「閉じる」を選んでクライアントを閉じる。
# ==================================================================
echo "--- (B) タイトルアイコンの左クリックでウィンドウメニューを開く ---"

frame_x=$(win_field "$frame_hex" "Absolute upper-left X")
frame_y=$(win_field "$frame_hex" "Absolute upper-left Y")
case "$frame_x" in '' | *[!0-9-]*) fail "frame ($frame_hex) の絶対座標Xを取得できませんでした" ;; esac
case "$frame_y" in '' | *[!0-9-]*) fail "frame ($frame_hex) の絶対座標Yを取得できませんでした" ;; esac

icon_x=$((frame_x + BORDER + ICON_OFFSET + ICON_SIZE / 2))
icon_y=$((frame_y + BORDER + CAPTION_H / 2))

list_root_children >"$WORKDIR/baseline_b"

xdotool mousemove --sync "$icon_x" "$icon_y" click --clearmodifiers 1 >/dev/null 2>&1 ||
	fail "タイトルアイコン ($icon_x,$icon_y) への左クリック送出に失敗しました"

menu2=$(wait_new_child "$WORKDIR/baseline_b" 5) || fail "タイトルアイコンの左クリックの後、新しいウィンドウ (メニュー) が現れませんでした (クリック座標: $icon_x,$icon_y)"
echo "OK: タイトルアイコンの左クリックでウィンドウ $menu2 が現れました"

ov2=$(win_field "$menu2" "Override Redirect State")
ov2_lc=$(printf '%s' "$ov2" | tr 'A-Z' 'a-z')
if [ "$ov2_lc" != "yes" ]; then
	echo "FAIL: メニューウィンドウ ($menu2) の Override Redirect State が yes ではありません (実際: '$ov2')" >&2
	FAILED=1
fi

menu_x=$(win_field "$menu2" "Absolute upper-left X")
menu_y=$(win_field "$menu2" "Absolute upper-left Y")
menu_w=$(win_field "$menu2" "Width")
menu_h=$(win_field "$menu2" "Height")
case "$menu_x$menu_y$menu_w$menu_h" in
*[!0-9-]*|'')
	fail "メニューウィンドウ ($menu2) のジオメトリを取得できませんでした (x=$menu_x y=$menu_y w=$menu_w h=$menu_h)"
	;;
esac

# 「閉じる」は SPEC §4.6 の順序で最後の項目。メニュー内部のレイアウトを
# 仮定せず、実測したメニュー全体の高さから「一番下の項目の行」の
# 中心付近をクリックする (項目高 MENU_ITEM_H の半分を底から差し引く)。
close_x=$((menu_x + menu_w / 2))
close_y=$((menu_y + menu_h - MENU_ITEM_H / 2))

xdotool mousemove --sync "$close_x" "$close_y" click --clearmodifiers 1 >/dev/null 2>&1 ||
	fail "「閉じる」項目 ($close_x,$close_y) への左クリック送出に失敗しました"

closed=0
i=0
while [ $i -lt 25 ]; do
	if ! client_in_list; then
		closed=1
		break
	fi
	sleep 0.2
	i=$((i + 1))
done

if [ "$closed" -eq 1 ]; then
	echo "OK: 「閉じる」を選んだ後、xterm ($winhex) が _NET_CLIENT_LIST から消えました"
else
	echo "FAIL: 「閉じる」をクリック ($close_x,$close_y) した後も xterm ($winhex) が _NET_CLIENT_LIST に残っています" >&2
	FAILED=1
fi

if [ "$FAILED" -ne 0 ]; then
	fail "1つ以上のウィンドウメニュー検証に失敗しました (詳細は上記の FAIL 行)"
fi

echo "OK: ウィンドウメニューの開閉 (Alt+Space / アイコン左クリック / Esc / 閉じる選択) が SPEC §4.6 の通り動作しました"
exit 0
