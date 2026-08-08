#!/bin/sh
#
# screenshot.sh - Phase 2 目視確認用のスクリーンショット取得ツール
#
# 概要:
#   自前で Xvfb + w98wm を起動し、xterm を1〜2枚開いて画面が落ち着くのを
#   待ってから、ルートウィンドウを xwd で撮って PNG に変換する。
#   人間が「見た目」を判断するための画像を作るのが目的なので、
#   静かに・確実に・後始末をきちんとすることを優先する。
#
#   Phase 2 (theme.c / draw.c / font.c / deco.c / cursor.c / menu.c) は
#   他エージェントが並行して実装中で、未着手/半完成のことがある。
#   このスクリプト自体は Phase 1 の w98wm (装飾なし) に対しても問題なく動く
#   ---装飾が描かれていない素のフレームが写るだけで、それはそれで正しい
#   結果である。deco.c 等が実装され次第、そのまま Win98 の見た目を
#   確認する道具として使えるようになる。
#
# 使い方:
#   tools/screenshot.sh [オプション]
#     --out PATH        出力 PNG のパス (既定: /tmp/w98wm-shot.png)
#     --clients N        開く xterm の枚数。1 か 2 (既定: 1)
#     --geometry GEOM     xterm 1枚目のジオメトリ (既定: 80x24+40+40)
#                          xterm の -geometry と同じ書式 (WxH+X+Y)。
#                          2枚目を開く場合は X/Y に +40+40 ずらして配置する。
#     --screen WxHxD      Xvfb の画面サイズ・色深度 (既定: 1280x1024x24)
#     --wait SECONDS      マップ後に追加で待つ秒数 (既定: 1)
#     --bin PATH          w98wm バイナリのパス (既定: ./w98wm または $W98WM_BIN)
#     --keep-xwd           中間生成物の .xwd ファイルを残す (デバッグ用)
#     -h, --help          このヘルプを表示
#
# 終了コード:
#   0   PNG を正常に生成した
#   1   ツール不足・起動失敗など、PNG を生成できなかった
#   2   引数エラー
#
# シェル: POSIX sh (dash で動作確認済み)。
#
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

OUT="/tmp/w98wm-shot.png"
CLIENTS=1
GEOMETRY="80x24+40+40"
SCREEN="1280x1024x24"
WAIT_SECS=1
BIN="${W98WM_BIN:-$ROOT_DIR/w98wm}"
KEEP_XWD=0

usage() {
	sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
	--out)
		OUT="$2"
		shift 2
		;;
	--clients)
		CLIENTS="$2"
		shift 2
		;;
	--geometry)
		GEOMETRY="$2"
		shift 2
		;;
	--screen)
		SCREEN="$2"
		shift 2
		;;
	--wait)
		WAIT_SECS="$2"
		shift 2
		;;
	--bin)
		BIN="$2"
		shift 2
		;;
	--keep-xwd)
		KEEP_XWD=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "不明なオプション: $1" >&2
		exit 2
		;;
	esac
done

case "$CLIENTS" in
1 | 2) : ;;
*)
	echo "エラー: --clients は 1 か 2 のみ対応しています (指定値: $CLIENTS)" >&2
	exit 2
	;;
esac

# --- 必要なツールの確認 (無ければ穏やかに諦める) ---
MISSING=""
for tool in Xvfb xterm xwd convert; do
	command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
if [ -n "$MISSING" ]; then
	echo "エラー: 以下のツールが見つかりません:$MISSING" >&2
	echo "        Xvfb / xterm / xwd (x11-apps 等) / convert (ImageMagick) が必要です。" >&2
	exit 1
fi

if [ ! -x "$BIN" ]; then
	echo "エラー: w98wm バイナリが見つかりません ($BIN)。" >&2
	echo "        先に 'make' を実行してください (他の担当が src/ を実装中の可能性もあります)。" >&2
	exit 1
fi

WORKROOT=$(mktemp -d)
XVFB_PID=""
WM_PID=""
CLIENT_PIDS=""

_CLEANED_UP=0
cleanup() {
	[ "$_CLEANED_UP" -eq 1 ] && return 0
	_CLEANED_UP=1
	for p in $CLIENT_PIDS; do
		kill "$p" 2>/dev/null || true
	done
	if [ -n "$WM_PID" ]; then
		kill "$WM_PID" 2>/dev/null || true
	fi
	if [ -n "$XVFB_PID" ]; then
		kill "$XVFB_PID" 2>/dev/null || true
	fi
	wait 2>/dev/null || true
	if [ "$KEEP_XWD" -ne 1 ]; then
		rm -rf "$WORKROOT"
	else
		echo "中間生成物を残しました: $WORKROOT" >&2
	fi
}
trap cleanup EXIT INT TERM

find_free_display() {
	d=99
	while [ -e "/tmp/.X${d}-lock" ]; do
		d=$((d + 1))
	done
	echo "$d"
}

DISP=$(find_free_display)
DISPLAY=":$DISP"
export DISPLAY

Xvfb "$DISPLAY" -screen 0 "$SCREEN" -nolisten tcp >"$WORKROOT/xvfb.log" 2>&1 &
XVFB_PID=$!

i=0
while [ ! -e "/tmp/.X${DISP}-lock" ] && [ $i -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
if [ ! -e "/tmp/.X${DISP}-lock" ]; then
	echo "エラー: Xvfb の起動に失敗しました (ログ: $WORKROOT/xvfb.log)" >&2
	exit 1
fi

DISPLAY="$DISPLAY" "$BIN" >"$WORKROOT/wm.log" 2>&1 &
WM_PID=$!
sleep 0.5
if ! kill -0 "$WM_PID" 2>/dev/null; then
	echo "エラー: w98wm が起動直後に終了しました (ログ: $WORKROOT/wm.log)" >&2
	cat "$WORKROOT/wm.log" >&2 || true
	exit 1
fi

# --geometry の X/Y 部分を読み取って2枚目用にずらす (無ければそのまま複製)
shift_geometry() {
	g="$1"
	off="$2"
	case "$g" in
	*+*+*)
		wh=$(printf '%s' "$g" | sed -n 's/^\([^+]*\)+.*/\1/p')
		x=$(printf '%s' "$g" | sed -n 's/^[^+]*+\([0-9-]*\)+.*/\1/p')
		y=$(printf '%s' "$g" | sed -n 's/^[^+]*+[0-9-]*+\([0-9-]*\)$/\1/p')
		printf '%s+%s+%s\n' "$wh" "$((x + off))" "$((y + off))"
		;;
	*)
		printf '%s\n' "$g"
		;;
	esac
}

xterm -display "$DISPLAY" -geometry "$GEOMETRY" -T "w98wm-screenshot-1" >/dev/null 2>&1 &
p1=$!
CLIENT_PIDS="$CLIENT_PIDS $p1"

if [ "$CLIENTS" -eq 2 ]; then
	geom2=$(shift_geometry "$GEOMETRY" 40)
	xterm -display "$DISPLAY" -geometry "$geom2" -T "w98wm-screenshot-2" >/dev/null 2>&1 &
	p2=$!
	CLIENT_PIDS="$CLIENT_PIDS $p2"
fi

# xterm がマップされ WM に管理されるまで待つ (xdotool があれば使う。無ければ固定待機)
if command -v xdotool >/dev/null 2>&1; then
	j=0
	while [ $j -lt 50 ]; do
		n=$(xdotool search --onlyvisible --name '.*' 2>/dev/null | wc -l | tr -d ' ')
		if [ "${n:-0}" -ge "$CLIENTS" ] 2>/dev/null; then
			break
		fi
		sleep 0.1
		j=$((j + 1))
	done
else
	sleep 1.5
fi

# 落ち着くまでの追加待機 (装飾の初回描画・Expose 処理など)
sleep "$WAIT_SECS"

XWD_FILE="$WORKROOT/root.xwd"
if ! xwd -display "$DISPLAY" -root -silent -out "$XWD_FILE" 2>"$WORKROOT/xwd.log"; then
	echo "エラー: xwd によるキャプチャに失敗しました (ログ: $WORKROOT/xwd.log)" >&2
	exit 1
fi

OUT_DIR=$(dirname -- "$OUT")
mkdir -p "$OUT_DIR"

if ! convert "$XWD_FILE" "$OUT" 2>"$WORKROOT/convert.log"; then
	echo "エラー: convert による PNG 変換に失敗しました (ログ: $WORKROOT/convert.log)" >&2
	exit 1
fi

if [ ! -s "$OUT" ]; then
	echo "エラー: $OUT が生成されませんでした、または空です。" >&2
	exit 1
fi

echo "OK: スクリーンショットを書き出しました: $OUT"
echo "    DISPLAY=$DISPLAY / クライアント数=$CLIENTS / geometry=$GEOMETRY"
exit 0
