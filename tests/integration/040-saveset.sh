#!/bin/sh
#
# 040-saveset.sh - 最重要テスト (SPEC §3.1.1, §10-7)
#
# ChangeSaveSet(INSERT) が正しく実装されているかを検証する唯一のテスト。
#
# 手順: xterm を WM 管理下に置いてから、WM プロセスを kill -9 する。
# セーブセットが機能していれば、X サーバが xterm のウィンドウを自動的に
# ルート直下へ reparent し直して残してくれる。機能していなければ、
# frame ウィンドウの DestroyWindow が子孫(xterm)を再帰的に道連れにして消える。
#
# **必ず SIGKILL (-9) を使うこと。** SIGTERM では WM の正常終了パス
# (自前でルートへ reparent し直す処理) が走ってしまい、セーブセットの
# 実装漏れがあってもこのテストは (誤って) パスしてしまう。
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

root=$(get_root_window)

# 前提条件: kill する前に、そもそも reparent されていることを確認しておく
# (されていなければ、この後の「ルートに戻っているか」の検証自体が無意味になる)。
parent_before=$(get_parent_window "$winhex")
if [ -n "$parent_before" ] && [ -n "$root" ]; then
	[ "$parent_before" != "$root" ] ||
		fail "前提条件エラー: xterm ($winhex) が kill -9 の前からルート直下です (reparent されていない)"
fi

# --- 本題: SIGTERM ではなく SIGKILL (-9) で WM を落とす ---
kill -9 "$WM_PID" 2>/dev/null || true

i=0
while kill -0 "$WM_PID" 2>/dev/null && [ $i -lt 25 ]; do
	sleep 0.2
	i=$((i + 1))
done
sleep 0.3

# xterm プロセス自体が生きているか (セーブセットが無ければ WM の
# DestroyWindow がウィンドウを、ひいては xterm クライアントを巻き添えにする)
if ! kill -0 "$XTERM_PID" 2>/dev/null; then
	fail "WM を kill -9 した後、xterm プロセスも終了しました (セーブセット未実装の疑い)"
fi

# ウィンドウ自体がまだ X サーバ上に存在するか
if ! xdotool getwindowname "$win" >/dev/null 2>&1; then
	fail "WM を kill -9 した後、ウィンドウ ($winhex) が X サーバ上から消えています (セーブセット未実装の疑い)"
fi

parent_after=$(get_parent_window "$winhex")
if [ -n "$parent_after" ] && [ -n "$root" ]; then
	[ "$parent_after" = "$root" ] ||
		fail "kill -9 後、ウィンドウ ($winhex) がルートへ reparent されていません (親=$parent_after, root=$root)。ChangeSaveSet(INSERT) 漏れの疑いがあります"
	echo "OK: kill -9 後、ウィンドウ ($winhex) はルート ($root) へ reparent され、xterm も生存しています"
else
	echo "注記: xwininfo が使えないため親ウィンドウの直接確認ができません。"
	echo "OK (簡易確認): kill -9 後もウィンドウと xterm プロセスは生存しています"
fi

exit 0
