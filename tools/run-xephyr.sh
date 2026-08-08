#!/bin/sh
#
# run-xephyr.sh - ネストした X サーバ上で w98wm を起動し、目視・対話的な検証を行う。
#
# 優先順位: Xephyr があれば Xephyr を使う。無ければ Xvfb にフォールバックし、
# 画面を見る手段 (x11vnc / スクリーンショット) を案内する (SPEC の検証戦略・
# PLAN Phase 0 の「Xephyr 等の検証ツールが無い場合」の対策に対応)。
#
# 使い方:
#   tools/run-xephyr.sh [--geometry=1024x768] [--no-clients]
#
# 終了時は Ctrl-C (SIGINT) や SIGTERM で、起動した子プロセス
# (ネストXサーバ・WM・テストクライアント) をすべて後始末する。

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT_DIR/w98wm"

GEOM="1024x768"
SPAWN_CLIENTS=1

for arg in "$@"; do
	case "$arg" in
	--geometry=*)
		GEOM="${arg#--geometry=}"
		;;
	--no-clients)
		SPAWN_CLIENTS=0
		;;
	-h | --help)
		echo "使い方: $0 [--geometry=WxH] [--no-clients]"
		echo "  --geometry=WxH  ネストしたXサーバの画面サイズ (既定: 1024x768)"
		echo "  --no-clients    xterm/xclock 等のテストクライアントを起動しない"
		exit 0
		;;
	*)
		echo "不明なオプション: $arg" >&2
		exit 2
		;;
	esac
done

case "$GEOM" in
[0-9]*x[0-9]*) ;;
*)
	echo "エラー: --geometry は WxH の形式で指定してください (例: 1024x768)" >&2
	exit 2
	;;
esac

if [ ! -x "$BIN" ]; then
	echo "エラー: $BIN が見つかりません。" >&2
	echo "先に 'make' でビルドしてから再度実行してください。" >&2
	exit 1
fi

PIDS=""
_CLEANED_UP=0
cleanup() {
	# EXIT と INT/TERM の両方に trap を張っているため、シグナル受信後に
	# 自然に EXIT へ抜けると二重に走る。ガード変数で1回だけにする。
	[ "$_CLEANED_UP" -eq 1 ] && return 0
	_CLEANED_UP=1
	echo ""
	echo "終了処理中..."
	for p in $PIDS; do
		kill "$p" 2>/dev/null || true
	done
	wait 2>/dev/null || true
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

if command -v Xephyr >/dev/null 2>&1; then
	BACKEND="Xephyr"
	Xephyr "$DISPLAY" -screen "$GEOM" -ac -br -reset -title "w98wm (Xephyr $DISPLAY)" >/tmp/w98wm-xephyr-$$.log 2>&1 &
	NEST_PID=$!
	PIDS="$PIDS $NEST_PID"
elif command -v Xvfb >/dev/null 2>&1; then
	BACKEND="Xvfb"
	echo "注記: Xephyr が見つからないため Xvfb にフォールバックします。"
	echo "      画面を見るには次のいずれかを使ってください:"
	echo "        x11vnc -display $DISPLAY -localhost -once   (別ターミナルで VNC 接続)"
	echo "        import -display $DISPLAY -window root shot.png   (ImageMagick でスクリーンショット)"
	echo "        xwd -display $DISPLAY -root -out shot.xwd         (xwd でも可)"
	Xvfb "$DISPLAY" -screen 0 "${GEOM}x24" -nolisten tcp >/tmp/w98wm-xvfb-$$.log 2>&1 &
	NEST_PID=$!
	PIDS="$PIDS $NEST_PID"
else
	echo "エラー: Xephyr も Xvfb も見つかりません。どちらか一方をインストールしてください。" >&2
	exit 1
fi

i=0
while [ ! -e "/tmp/.X${DISP}-lock" ] && [ $i -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
if [ ! -e "/tmp/.X${DISP}-lock" ]; then
	echo "エラー: $BACKEND の起動に失敗しました。" >&2
	exit 1
fi

echo "$BACKEND を DISPLAY=$DISPLAY (${GEOM}) で起動しました。"

"$BIN" >/tmp/w98wm-run-$$.log 2>&1 &
WM_PID=$!
PIDS="$PIDS $WM_PID"

sleep 1
if ! kill -0 "$WM_PID" 2>/dev/null; then
	echo "エラー: w98wm がすぐに終了しました。ログ: /tmp/w98wm-run-$$.log" >&2
	exit 1
fi
echo "w98wm を起動しました (pid=$WM_PID)。"

if [ "$SPAWN_CLIENTS" -eq 1 ]; then
	if command -v xterm >/dev/null 2>&1; then
		xterm -display "$DISPLAY" &
		PIDS="$PIDS $!"
		echo "テストクライアント: xterm を起動しました。"
	fi
	if command -v xclock >/dev/null 2>&1; then
		xclock -display "$DISPLAY" &
		PIDS="$PIDS $!"
		echo "テストクライアント: xclock を起動しました。"
	fi
fi

echo ""
echo "DISPLAY=$DISPLAY で $BACKEND 上に w98wm が動作しています。"
echo "終了するには Ctrl-C を押してください。"

wait "$WM_PID" 2>/dev/null || true
