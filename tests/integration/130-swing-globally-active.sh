#!/bin/sh
#
# 130-swing-globally-active.sh
#
# ICCCM §4.1.7 の **Globally Active** 入力モデルの検証（SPEC §5.1）。
#
# なぜ専用のテストが要るか
# ------------------------
# 入力モデルは 4 つあるが、実アプリで Globally Active
# (WM_HINTS.input = False かつ WM_TAKE_FOCUS あり) を使う実装はほぼ
# Java/AWT だけである。GTK も Qt も xterm も Passive なので、
# それらでいくら試してもこの経路には入らない。
#
# 取り違えると症状は「クリックしてもキー入力が入らないウィンドウ」で、
# WM 側にはエラーも警告も一切出ない。Swing アプリを開いた人が
# 初めて気づく類の壊れ方をする。したがってここだけは
# **本物の Java アプリを起動して端から端まで通す**しかない。
#
# 検証項目:
#   1. Java の窓が本当に Globally Active である
#      (前提が崩れたら以降の判定に意味が無いので skip する)
#   2. WM が管理下に入れ、_NET_ACTIVE_WINDOW を立てる
#   3. X の入力フォーカスがクライアントウィンドウ自身にある
#   4. **打鍵がアプリまで届く** (アプリが KEY: を標準出力に出す)
#   5. 他の窓へフォーカスを移すと FOCUS:lost、戻すと再び届く
#
# java が無い環境では skip (exit 77)。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v java >/dev/null 2>&1 || skip "java がありません"
command -v javac >/dev/null 2>&1 || skip "javac がありません"
require_tool xterm

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT_DIR/tools/SwingTest.java"
[ -f "$SRC" ] || skip "tools/SwingTest.java がありません"

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

javac -d "$TMP/classes" "$SRC" >"$TMP/javac.log" 2>&1 ||
	skip "SwingTest.java をコンパイルできません: $(tail -3 "$TMP/javac.log")"

start_xvfb
start_wm

# ヘッドレス扱いにされると窓が出ないので明示的に切る
java -Djava.awt.headless=false -cp "$TMP/classes" SwingTest \
	>"$TMP/app.out" 2>"$TMP/app.err" &
JAVA_PID=$!
_LIB_PIDS="$_LIB_PIDS $JAVA_PID"

#
# ★ 窓の特定に xdotool search の先頭を使ってはいけない。
#   Java は表に出ない補助ウィンドウをいくつか作るので、名前で引くと
#   そちらが先に当たる（実際に当たって WM_HINTS が読めず、
#   「この JDK は対象外」という誤った skip になっていた）。
#   **WM が管理下に入れた窓**、つまり _NET_CLIENT_LIST に載っていて
#   かつ名前が一致するものを選ぶ。
#
find_managed_swing() {
	for id in $(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
			sed 's/.*# *//' | tr ',' ' '); do
		name=$(xprop -display "$DISPLAY" -id "$id" WM_NAME _NET_WM_NAME 2>/dev/null || true)
		case "$name" in
		*Swing*)
			printf '%d\n' "$id"
			return 0
			;;
		esac
	done
	return 1
}

win=""
i=0
while [ $i -lt 125 ]; do
	win=$(find_managed_swing) && [ -n "$win" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$win" ] ||
	fail "Swing の窓が管理下に入りませんでした: $(tail -5 "$TMP/app.err" 2>/dev/null)"
sleep 2

# ------------------------------------------------------------------
# 1. 本当に Globally Active か（前提の確認）
#
#    プロパティが**読めない**のは前提の不一致ではなく異常なので fail。
#    skip にすると「テストが緑なのに何も検査していない」状態を作る。
# ------------------------------------------------------------------
hints=$(xprop -display "$DISPLAY" -id "$win" WM_HINTS 2>/dev/null || true)
protos=$(xprop -display "$DISPLAY" -id "$win" WM_PROTOCOLS 2>/dev/null || true)

echo "$hints" | grep -q 'input focus' ||
	fail "WM_HINTS が読めません (win=$win): $hints"
echo "$hints" | grep -q 'input focus: False' ||
	skip "この JDK は Globally Active ではありません。判定対象外: $hints"
echo "$protos" | grep -q 'WM_TAKE_FOCUS' ||
	skip "この JDK は WM_TAKE_FOCUS を持ちません。判定対象外: $protos"

# ------------------------------------------------------------------
# 2. _NET_ACTIVE_WINDOW が立つ
# ------------------------------------------------------------------
winhex=$(to_hex "$win")

xdotool windowactivate --sync "$win" >/dev/null 2>&1 || true
sleep 1.5

active=$(xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null | sed 's/.*# *//')
case "$active" in
*"$(printf '%x' "$win")"*) ;;
*) fail "_NET_ACTIVE_WINDOW が Swing の窓になっていません (active=$active, win=$winhex)" ;;
esac

# ------------------------------------------------------------------
# 3. X の入力フォーカスがクライアントウィンドウ自身にある
#
#    Globally Active では WM が SetInputFocus を呼ばず、アプリが
#    WM_TAKE_FOCUS を受けて自分で取りに行く。結果としてフォーカスは
#    frame ではなく **クライアントウィンドウ** に載る。
# ------------------------------------------------------------------
focused=$(xdotool getwindowfocus 2>/dev/null || echo 0)
[ "$focused" = "$win" ] ||
	fail "入力フォーカスが Swing のクライアントウィンドウにありません (focus=$focused, win=$win)"

grep -q '^FOCUS:gained' "$TMP/app.out" ||
	fail "アプリが WindowGainedFocus を受け取っていません（WM_TAKE_FOCUS が届いていない）"

# ------------------------------------------------------------------
# 4. 打鍵がアプリまで届く（ここが本題）
# ------------------------------------------------------------------
xdotool key --clearmodifiers a
xdotool key --clearmodifiers b
xdotool key --clearmodifiers c
sleep 1.5

got=$(grep -c '^KEY:' "$TMP/app.out" 2>/dev/null || echo 0)
[ "${got:-0}" -ge 3 ] ||
	fail "打鍵がアプリに届いていません (KEY: の行数=$got)。フォーカスは渡っているのに入力が入らない = Globally Active を Passive として扱っている疑い"

# ------------------------------------------------------------------
# 5. 他の窓へ移して戻す
# ------------------------------------------------------------------
xterm -display "$DISPLAY" -geometry 30x8+600+400 -T w98wm-swing-other >/dev/null 2>&1 &
XT=$!
_LIB_PIDS="$_LIB_PIDS $XT"
other=$(wait_for_window --pid "$XT" 10) || fail "xterm が現れませんでした"
sleep 1

xdotool windowactivate --sync "$other" >/dev/null 2>&1 || true
sleep 1.5
grep -q '^FOCUS:lost' "$TMP/app.out" ||
	fail "他の窓をアクティブにしても Swing 側が FOCUS:lost を受け取っていません"

before=$(grep -c '^KEY:' "$TMP/app.out" 2>/dev/null || echo 0)
xdotool windowactivate --sync "$win" >/dev/null 2>&1 || true
sleep 1.5
xdotool key --clearmodifiers d
sleep 1.5
after=$(grep -c '^KEY:' "$TMP/app.out" 2>/dev/null || echo 0)
[ "${after:-0}" -gt "${before:-0}" ] ||
	fail "戻ってきた後に打鍵が届きません ($before -> $after)。フォーカスの再取得が壊れている"

echo "# Globally Active: WM_TAKE_FOCUS の受け渡しと打鍵の到達を確認 (KEY: $after 行)"
exit 0
