#!/bin/sh
#
# lib.sh - 結合テスト共通ヘルパ
#
# 各 tests/integration/*.sh の先頭で以下のように読み込んで使う:
#   . "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"
#
# 設計方針:
#   各テストスクリプトは「自分専用の」フレッシュな Xvfb + WM インスタンスを
#   起動する (start_xvfb / start_wm)。テスト間で Xvfb・WM を共有しない。
#   これは 040-saveset.sh が WM プロセス自体を kill -9 で落とすテストであり、
#   共有した場合は後続のテストが巻き添えで失敗するため。
#   tests/run-tests.sh 側は各スクリプトを個別プロセスとして実行するだけで、
#   Xvfb/WM のライフサイクル管理はしない。
#
# 必要なツールが無い場合は skip (終了コード 77) で降りる。
# w98wm バイナリが無い場合も同様に skip する (他エージェントがまだ
# src/ を実装中でビルドが終わっていない可能性があるため、fail 扱いにはしない)。
#
# POSIX sh 前提 (dash で動作確認)。

skip() {
	echo "SKIP: $*"
	exit 77
}

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

require_tool() {
	command -v "$1" >/dev/null 2>&1 || skip "必要なツールがありません: $1"
}

require_tool xdotool
require_tool xprop
require_tool Xvfb

WM_BIN="${W98WM_BIN:-$(pwd)/w98wm}"
if [ ! -x "$WM_BIN" ]; then
	skip "w98wm バイナリが見つかりません ($WM_BIN)。ビルドされていないようです。"
fi

_LIB_PIDS=""
_LIB_CLEANED_UP=0
#
# 後始末。**SIGTERM のあと必ず SIGKILL まで落とす。**
#
# SIGTERM を送って親シェルがすぐ終了すると、まだ終了処理中の子
# (特に Xvfb) が取り残される。実際にこのセッションで w98wm が 17 個、
# Xvfb が 24 個生き残っていた。CI で繰り返すと積み上がる。
#
_lib_cleanup() {
	[ "$_LIB_CLEANED_UP" -eq 1 ] && return 0
	_LIB_CLEANED_UP=1

	for p in $_LIB_PIDS; do
		kill "$p" 2>/dev/null || true
	done

	# 終了を少し待ってから、残っているものに止めを刺す
	i=0
	while [ $i -lt 20 ]; do
		alive=0
		for p in $_LIB_PIDS; do
			kill -0 "$p" 2>/dev/null && alive=1
		done
		[ "$alive" -eq 0 ] && break
		sleep 0.1
		i=$((i + 1))
	done
	for p in $_LIB_PIDS; do
		kill -9 "$p" 2>/dev/null || true
	done

	wait 2>/dev/null || true
}
trap _lib_cleanup EXIT INT TERM

find_free_display() {
	d=99
	while [ -e "/tmp/.X${d}-lock" ]; do
		d=$((d + 1))
	done
	echo "$d"
}

# start_xvfb: フレッシュな Xvfb を起動し、DISPLAY を export する。
start_xvfb() {
	disp=$(find_free_display)
	Xvfb ":$disp" -screen 0 1280x1024x24 -nolisten tcp >/tmp/w98wm-test-xvfb-$$.log 2>&1 &
	XVFB_PID=$!
	_LIB_PIDS="$_LIB_PIDS $XVFB_PID"
	i=0
	while [ ! -e "/tmp/.X${disp}-lock" ] && [ $i -lt 50 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -e "/tmp/.X${disp}-lock" ] || fail "Xvfb の起動に失敗しました"

	DISPLAY=":$disp"
	export DISPLAY

	#
	# ★ ロックファイルができた ≠ 接続を受け付けられる。
	#
	#   ロックは Xvfb が起動の**最初**に作る。実際にソケットで待ち受ける
	#   までには少し間があり、その隙間に start_wm() が走ると WM が
	#   接続に失敗して即座に終了する。テストからは
	#   「WM が起動直後に終了しました」に見える。
	#   負荷が低いときは間に合ってしまうので、**テストの本数が増えて
	#   初めて顕在化する**たちの悪い競合になる。実際に 18 本にしたところで
	#   毎回 2〜5 本が落ちるようになった。
	#
	#   実際にプロパティを 1 つ引けるまで待つ。
	#
	i=0
	while [ $i -lt 100 ]; do
		if xprop -display "$DISPLAY" -root >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.1
		i=$((i + 1))
	done
	fail "Xvfb が接続を受け付けません ($DISPLAY)"
}

# start_wm: $WM_BIN を起動し WM_PID を設定する。
# _NET_SUPPORTING_WM_CHECK が現れるまで最大 6 秒待つが、現れなくても
# (未実装の可能性があるため) fail にはせず先へ進める。以降の実際の
# アサーションで自然に失敗させる方針。
start_wm() {
	WM_PID_LOG="/tmp/w98wm-test-wm-$$.log"
	DISPLAY="$DISPLAY" "$WM_BIN" >"$WM_PID_LOG" 2>&1 &
	WM_PID=$!
	_LIB_PIDS="$_LIB_PIDS $WM_PID"
	i=0
	while [ $i -lt 30 ]; do
		if ! kill -0 "$WM_PID" 2>/dev/null; then
			fail "WM が起動直後に終了しました (ログ: $WM_PID_LOG)"
		fi
		# _NET_SUPPORTING_WM_CHECK だけでは早すぎる。これは ewmh_init() の
		# 冒頭で立つため、その後に書かれるルートプロパティ
		# (_NET_NUMBER_OF_DESKTOPS 等) はまだ無い。実際に 060 が
		# "nosuchatomonanywindow" で稀に落ちた。起動の最後まで待つ。
		if xprop -display "$DISPLAY" -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1 &&
			xprop -display "$DISPLAY" -root _NET_NUMBER_OF_DESKTOPS 2>/dev/null |
				grep -q '[0-9]'; then
			return 0
		fi
		sleep 0.2
		i=$((i + 1))
	done
	# ここに来るのは _NET_SUPPORTING_WM_CHECK がまだ実装されていない場合。
	# WM プロセス自体は生きているので、そのままテスト本体に進む。
	return 0
}

# wait_for_window <xdotool searchのキー> <値> [タイムアウト秒(既定5)]
# 見つかったウィンドウID (10進) を標準出力に書いて成功、見つからなければ失敗を返す。
wait_for_window() {
	key="$1"
	val="$2"
	tmo="${3:-5}"
	max=$((tmo * 5))
	i=0
	while [ $i -lt $max ]; do
		win=$(xdotool search "$key" "$val" 2>/dev/null | head -n1)
		if [ -n "$win" ]; then
			echo "$win"
			return 0
		fi
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

#
# wait_for_managed_window <WM_NAME に含まれる文字列> [タイムアウト秒(既定10)]
#
# **WM の管理下に入った**ウィンドウ (= _NET_CLIENT_LIST に載っているもの)
# だけを対象に、名前で引く。見つかれば 10 進のウィンドウ ID を返す。
#
# ★ 名前で引くのに xdotool search を使ってはいけない。
#   xdotool search はルートの子を無差別に走査するので、
#   アプリが作る表に出ない補助ウィンドウ (Java がいくつも作る) や、
#   **w98wm 自身のタスクバー/トレイ** まで返す。
#   前者は 130 で WM_HINTS が読めない誤検出を、
#   後者は memcheck の M20h で `xdotool windowkill` が
#   WM 自身の X 接続を切って計測を丸ごと壊す事故を起こした。
#
wait_for_managed_window() {
	pattern="$1"
	tmo="${2:-10}"
	max=$((tmo * 5))
	i=0
	while [ $i -lt $max ]; do
		for id in $(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
				sed 's/.*# *//' | tr ',' ' '); do
			nm=$(xprop -display "$DISPLAY" -id "$id" WM_NAME _NET_WM_NAME 2>/dev/null || true)
			case "$nm" in
			*"$pattern"*)
				printf '%d\n' "$id"
				return 0
				;;
			esac
		done
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

#
# wait_for_managed_window_by_pid <PID> [タイムアウト秒(既定10)]
#
# _NET_WM_PID で照合して、管理下に入ったウィンドウを待つ。
#
# ★ **xterm を名前で探してはいけない。**
#   `xterm -T なまえ` で付けた WM_NAME は、中のシェルが起動直後に
#   エスケープシーケンスで上書きする（プロンプトの "user@host: cwd" になる）。
#   名前で待つと永久に見つからず、「WM が応答しない」という誤った
#   結論に化ける（実際に 150 でそう見えた）。
#   PID なら上書きされない。
#
wait_for_managed_window_by_pid() {
	want="$1"
	tmo="${2:-10}"
	max=$((tmo * 5))
	i=0
	while [ $i -lt $max ]; do
		for id in $(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
				sed 's/.*# *//' | tr ',' ' '); do
			p=$(xprop -display "$DISPLAY" -id "$id" _NET_WM_PID 2>/dev/null |
				sed 's/.*= *//' | tr -d ' ')
			if [ -n "$p" ] && [ "$p" = "$want" ]; then
				printf '%d\n' "$id"
				return 0
			fi
		done
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

# get_root_window: ルートウィンドウID (16進, 0x...) を返す。xwininfo が無ければ空文字。
get_root_window() {
	command -v xwininfo >/dev/null 2>&1 || {
		echo ""
		return 1
	}
	xwininfo -display "$DISPLAY" -root 2>/dev/null | awk '/Window id:/ { print $4; exit }'
}

# get_parent_window <ウィンドウID> : 親ウィンドウID (16進) を返す。xwininfo が無ければ空文字。
get_parent_window() {
	command -v xwininfo >/dev/null 2>&1 || {
		echo ""
		return 1
	}
	# -children (または -tree) を付けないと "Parent window id:" 行が出ない。
	# 付け忘れると常に空文字を返し、呼び出し側の reparent 検証が
	# 「確認できないので代替へ」の分岐に落ちて**素通り**する。
	# テストが緑のまま中身を検査していない状態になるので注意。
	xwininfo -display "$DISPLAY" -id "$1" -children 2>/dev/null |
		awk '/Parent window id:/ { print $4; exit }'
}

# hex表示 <10進のウィンドウID> : xdotool の10進出力を xprop/wmctrl 用の0x...表記に変換する
to_hex() {
	printf '0x%x\n' "$1"
}
