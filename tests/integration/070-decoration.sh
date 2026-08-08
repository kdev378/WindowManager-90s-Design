#!/bin/sh
#
# 070-decoration.sh
#
# SPEC §4.1 (配色) / §4.3 (ベベル描画) / §4.4 (タイトルバー) の
# ピクセルレベルの検証。
#
# 参照画像 (tests/reference/) を用意しなくても、フレームウィンドウの
# 特定座標の色を直接読み、SPEC が定める固定値と比較することで
# 「ピクセル精度」を CI 上で自動検証できるようにするのが狙い。
# (tools/pixdiff.sh は参照画像との全体比較。こちらは参照画像なしで
#  検証できる、ピンポイントな座標の絶対値チェック)
#
# 検証する座標 (SPEC §4.3 のベベル並び。外→内):
#   frame (0,0)              outermost TL = light    #DFDFDF
#   frame (w-1,h-1)           outermost BR = dkshadow #000000
#   frame (1,1)                inner TL     = hilight  #FFFFFF
#   frame (w-2,h-2)             inner BR     = shadow   #808080
#   frame (2,2)                  face 2px開始 = face     #C0C0C0
#   caption 左端 (フォーカス時)                       = active_title_l #000080
#   caption 右端付近 (ボタン域より手前、フォーカス時)   ≒ active_title_r #1084D0 に近づく
#
# 座標計算に使う border=4px / caption_h=18px / ボタン 16x14px×3 は
# SPEC §4.2 の値をそのままこのテストの前提として使う
# (src/draw.h の struct metrics と同じ値)。deco.c の実装がこれと違う値を
# 使っていれば、このテストの前提が崩れて FAIL になる --- それ自体が
# 「実装が SPEC §4.2 のメトリクスからズレている」ことの検出になる。
#
# キャプション右端は本物の Win98 ではキャプションボタンに隠れるため、
# 「右端そのもの」ではなくボタン域より手前の安全な位置を見て、
# 左端 (#000080) より右端の色 (#1084D0) に近づいていることを確認する
# 「近似」チェックにとどめる (SPEC §4.4 の補間規則そのものの厳密一致は
# tools/pixdiff.sh + 基準スクリーンショットの役割であり、このテストの
# 役割ではない)。
#
# ImageMagick が無い/src/deco.c が未実装の場合は skip (exit 77) する。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
require_tool xterm
require_tool convert
require_tool xwd
require_tool xwininfo

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [ ! -f "$ROOT_DIR/src/deco.c" ]; then
	skip "src/deco.c がまだ実装されていません (Phase 2 進行中の可能性があります)"
fi

# --- SPEC §4.2 のメトリクス (96dpi, scale=1 前提) ---
BORDER=4
CAPTION_H=18
BTN_W=16
BTN_COUNT=3
BTN_GAP_BEFORE_CLOSE=2
BUTTON_AREA=$((BTN_W * BTN_COUNT + BTN_GAP_BEFORE_CLOSE))
SAFETY=6

start_xvfb
start_wm

# 十分に幅のある xterm を開く (キャプション右端のボタン域を避けて
# サンプリングできるだけの余白を確保するため)
xterm -display "$DISPLAY" -geometry 100x30+60+60 -T w98wm-deco-test >/dev/null 2>&1 &
XTERM_PID=$!
_LIB_PIDS="$_LIB_PIDS $XTERM_PID"

win=$(wait_for_window --pid "$XTERM_PID" 10) || fail "xterm のウィンドウが現れませんでした"
winhex=$(to_hex "$win")

# フォーカスさせる (アクティブ配色 active_title_* を検証するため)
xdotool windowactivate --sync "$win" >/dev/null 2>&1 || true
sleep 0.5

# lib.sh の get_parent_window() は "-children" を付けずに xwininfo を呼ぶため、
# 手元の xwininfo (x11-apps 由来) では "Parent window id:" 行自体が出ず、
# 常に空を返してしまう (020-map-manage.sh 等が持つフォールバックはこれを
# 前提にしている)。このテストは frame の実体そのものが要るので、
# "-children" 付きで自前に取得する。
find_frame_window() {
	xwininfo -display "$DISPLAY" -id "$1" -children 2>/dev/null |
		awk '/Parent window id:/ { print $4; exit }'
}

frame_hex=$(find_frame_window "$winhex")
[ -n "$frame_hex" ] || skip "xwininfo で frame ウィンドウを特定できません"

root=$(get_root_window)
if [ -n "$root" ] && [ "$frame_hex" = "$root" ]; then
	fail "xterm ($winhex) が reparent されていません (frame が root のままです)"
fi

frame_info=$(xwininfo -display "$DISPLAY" -id "$frame_hex" 2>/dev/null) ||
	fail "xwininfo で frame ($frame_hex) の情報が取れません"
FW=$(printf '%s\n' "$frame_info" | awk -F': *' '/^ *Width:/ {print $2; exit}')
FH=$(printf '%s\n' "$frame_info" | awk -F': *' '/^ *Height:/ {print $2; exit}')
case "$FW" in '' | *[!0-9]*) fail "frame ($frame_hex) の幅を取得できませんでした" ;; esac
case "$FH" in '' | *[!0-9]*) fail "frame ($frame_hex) の高さを取得できませんでした" ;; esac

if [ "$FW" -lt $((BORDER * 2 + 20)) ] || [ "$FH" -lt $((BORDER * 2 + CAPTION_H + 10)) ]; then
	fail "テスト前提エラー: frame が小さすぎます (${FW}x${FH})。xterm のジオメトリを見直してください"
fi

# --- キャプチャ (frame ウィンドウそのものを xwd で撮る) ---
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"; _lib_cleanup' EXIT INT TERM

XWD_FILE="$WORKDIR/frame.xwd"
PNG_FILE="$WORKDIR/frame.png"

xwd -display "$DISPLAY" -id "$frame_hex" -silent -out "$XWD_FILE" 2>"$WORKDIR/xwd.log" ||
	fail "xwd によるフレーム ($frame_hex) のキャプチャに失敗しました (ログ: $WORKDIR/xwd.log)"
convert "$XWD_FILE" "$PNG_FILE" 2>"$WORKDIR/convert.log" ||
	fail "convert による PNG 変換に失敗しました (ログ: $WORKDIR/convert.log)"

# get_pixel <x> <y> : #RRGGBB (6桁, 大文字化なし) を返す。読めなければ空文字。
# ImageMagick の txt: 出力例:
#   "0,0: (223,223,223,255)  #DFDFDFFF  srgba(223,223,223,1)"
# から #RRGGBB (アルファは無視) を取り出す。
get_pixel() {
	x="$1"
	y="$2"
	convert "$PNG_FILE" -crop 1x1+"$x"+"$y" +repage txt:- 2>/dev/null |
		sed -n 's/.*#\([0-9A-Fa-f]\{6\}\)\([0-9A-Fa-f]\{2\}\)\{0,1\}[^0-9A-Fa-f].*/\1/p' | head -n1
}

hex_upper() {
	printf '%s' "$1" | tr 'a-f' 'A-F'
}

# hex_rgb <#無しhex> : "R G B" (10進) を1行で返す
hex_rgb() {
	h="$1"
	r=$(printf '%d' "0x$(printf '%s' "$h" | cut -c1-2)")
	g=$(printf '%d' "0x$(printf '%s' "$h" | cut -c3-4)")
	b=$(printf '%d' "0x$(printf '%s' "$h" | cut -c5-6)")
	echo "$r $g $b"
}

# hex_distance <hexA> <hexB> : ユークリッド距離 (RGB空間、小数)
hex_distance() {
	rgb_a=$(hex_rgb "$1")
	rgb_b=$(hex_rgb "$2")
	awk -v a="$rgb_a" -v b="$rgb_b" 'BEGIN{
		split(a, A, " "); split(b, B, " ");
		dr = A[1]-B[1]; dg = A[2]-B[2]; db = A[3]-B[3];
		printf "%.3f\n", sqrt(dr*dr + dg*dg + db*db);
	}'
}

FAILED=0

# check_pixel_eq <x> <y> <期待hex(#無し)> <ラベル> : 完全一致を要求する
check_pixel_eq() {
	x="$1"
	y="$2"
	expected="$3"
	label="$4"
	actual=$(get_pixel "$x" "$y")
	if [ -z "$actual" ]; then
		echo "FAIL: [$label] frame座標 ($x,$y) のピクセルを読み取れませんでした" >&2
		FAILED=1
		return
	fi
	exp_u=$(hex_upper "$expected")
	act_u=$(hex_upper "$actual")
	if [ "$act_u" != "$exp_u" ]; then
		echo "FAIL: [$label] frame座標 ($x,$y) 期待=#$exp_u 実際=#$act_u" >&2
		FAILED=1
	else
		echo "OK: [$label] frame座標 ($x,$y) = #$act_u"
	fi
}

# check_pixel_approaches <x> <y> <目標hex> <逆側hex> <許容絶対距離> <ラベル>
#   目標色との距離が逆側の色との距離より近く、かつ許容絶対距離以内であることを確認する
#   (グラデーションの「近づいている」ことの近似チェック。厳密な線形補間の
#    一致要求は tools/pixdiff.sh + 基準スクリーンショットの役割)
check_pixel_approaches() {
	x="$1"
	y="$2"
	target="$3"
	other="$4"
	max_dist="$5"
	label="$6"
	actual=$(get_pixel "$x" "$y")
	if [ -z "$actual" ]; then
		echo "FAIL: [$label] frame座標 ($x,$y) のピクセルを読み取れませんでした" >&2
		FAILED=1
		return
	fi
	act_u=$(hex_upper "$actual")
	tgt_u=$(hex_upper "$target")
	oth_u=$(hex_upper "$other")
	d_target=$(hex_distance "$act_u" "$tgt_u")
	d_other=$(hex_distance "$act_u" "$oth_u")
	ok=$(awk -v dt="$d_target" -v do_="$d_other" -v m="$max_dist" \
		'BEGIN{ print (dt < do_ && dt <= m) ? 1 : 0 }')
	if [ "$ok" -eq 1 ]; then
		echo "OK: [$label] frame座標 ($x,$y) = #$act_u (目標 #$tgt_u への距離 $d_target, 許容 $max_dist 以内)"
	else
		echo "FAIL: [$label] frame座標 ($x,$y) = #$act_u が目標 #$tgt_u に近づいていません (距離: 目標=$d_target / 反対側=$d_other, 許容=$max_dist)" >&2
		FAILED=1
	fi
}

echo "--- フレームのベベル (SPEC §4.3) FW=$FW FH=$FH ---"
check_pixel_eq 0 0 "DFDFDF" "外側ベベル TL = light"
check_pixel_eq 1 1 "FFFFFF" "内側ベベル TL = hilight"
check_pixel_eq 2 2 "C0C0C0" "フェース開始 (2,2) = face"

BR_X=$((FW - 1))
BR_Y=$((FH - 1))
check_pixel_eq "$BR_X" "$BR_Y" "000000" "外側ベベル BR = dkshadow"
check_pixel_eq $((FW - 2)) $((FH - 2)) "808080" "内側ベベル BR = shadow"

echo "--- タイトルバーのグラデーション (SPEC §4.1 / §4.4, フォーカス中) ---"
CAP_Y=$((BORDER + (CAPTION_H / 2)))
CAP_LEFT_X=$BORDER
check_pixel_eq "$CAP_LEFT_X" "$CAP_Y" "000080" "キャプション左端 = active_title_l"

CAP_RIGHT_SAFE_X=$((FW - BORDER - BUTTON_AREA - SAFETY))
if [ "$CAP_RIGHT_SAFE_X" -le $((CAP_LEFT_X + 20)) ]; then
	echo "SKIP: [キャプション右端寄り] frame幅 ($FW) が狭く、ボタン域を避けたサンプリング座標を確保できません"
else
	check_pixel_approaches "$CAP_RIGHT_SAFE_X" "$CAP_Y" "1084D0" "000080" 150 \
		"キャプション右端寄り(ボタン域の手前) ≒ active_title_r に近づく"
fi

if [ "$FAILED" -ne 0 ]; then
	fail "1つ以上のピクセル検証に失敗しました (詳細は上記の FAIL 行)"
fi

echo "OK: フレーム ($frame_hex, ${FW}x${FH}) のベベル・キャプション配色が SPEC §4.1/§4.3/§4.4 と一致しました"
exit 0
