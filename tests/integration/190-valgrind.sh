#!/bin/sh
#
# 190-valgrind.sh
#
# valgrind (memcheck) の下で起動 → 窓の管理 → 最小化/復元 → 終了まで回し、
# 未初期化メモリの使用・不正アクセス・リークが 0 であることを確かめる。
#
# 外部テスト（2026-10-27）の所見 6 で「writev に未初期化バイト」が 1 件出た。
# --track-origins=yes で辿ると出どころは libxcb-randr の
# xcb_randr_get_monitors() の要求構造体（末尾パディング）で、
# tools/valgrind.supp に理由つきで抑制してある。
# **それ以外が 1 件でも出たら落とす。**
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

require_tool valgrind
require_tool xterm

# musl では valgrind が libc 内部の最適化された文字列関数などを置き換えられず、
# w98wm と無関係の誤検知が出る。判定は glibc でだけ行う（CI の glibc ジョブ）。
if ldd --version 2>&1 | grep -qi musl; then
	skip "musl 上の valgrind は libc 内部の誤検知が多いので glibc でのみ判定する"
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SUPP="$ROOT/tools/valgrind.supp"
[ -f "$SUPP" ] || fail "抑制ファイルがありません: $SUPP"

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP" 2>/dev/null || true; }
# trap は後勝ち。lib.sh の _lib_cleanup と必ず合成する
trap '_lib_cleanup; cleanup_tmp' EXIT INT TERM

# start_wm は $WM_BIN をそのまま起動するので、valgrind を挟む小さな包みを渡す。
# exec するので PID は valgrind のものになり、_lib_cleanup がそのまま止められる。
REAL_BIN="$WM_BIN"
cat >"$TMP/wm-under-valgrind" <<EOF
#!/bin/sh
exec valgrind --quiet --error-exitcode=99 --leak-check=full \\
	--errors-for-leak-kinds=definite --suppressions="$SUPP" \\
	--log-file="$TMP/vg.log" "$REAL_BIN" "\$@"
EOF
chmod +x "$TMP/wm-under-valgrind"
WM_BIN="$TMP/wm-under-valgrind"

start_xvfb
start_wm

# valgrind の下では起動が数倍遅い。管理を始めるまで待つ
i=0
while [ $i -lt 100 ]; do
	xprop -display "$DISPLAY" -root _NET_NUMBER_OF_DESKTOPS 2>/dev/null | grep -q '[0-9]' && break
	kill -0 "$WM_PID" 2>/dev/null || { cat "$TMP/vg.log" 2>/dev/null; fail "valgrind 下の WM が起動中に終了しました"; }
	sleep 0.2
	i=$((i + 1))
done

xterm -display "$DISPLAY" -geometry 40x10+60+60 >/dev/null 2>&1 &
XT=$!
_LIB_PIDS="$_LIB_PIDS $XT"
win=$(wait_for_managed_window_by_pid "$XT" 40) || fail "xterm が管理下に入りません"

# 一通りの経路を踏ませる: 移動・最小化・復元・閉じる
xdotool windowmove "$win" 200 150 >/dev/null 2>&1 || true
sleep 0.5
xdotool windowminimize "$win" >/dev/null 2>&1 || true
sleep 1
xdotool windowactivate "$win" >/dev/null 2>&1 || true
sleep 1
kill "$XT" 2>/dev/null || true
sleep 1

# 正常終了させてからログを見る（SIGTERM → クリーンな終了処理）
kill -TERM "$WM_PID" 2>/dev/null || true
rc=0
i=0
while kill -0 "$WM_PID" 2>/dev/null && [ $i -lt 100 ]; do
	sleep 0.2
	i=$((i + 1))
done
kill -0 "$WM_PID" 2>/dev/null && fail "valgrind 下の WM が SIGTERM で終了しません"
wait "$WM_PID" 2>/dev/null || rc=$?

if [ "$rc" -eq 99 ] || grep -q "ERROR SUMMARY: [1-9]" "$TMP/vg.log" 2>/dev/null ||
		grep -q "definitely lost: [1-9]" "$TMP/vg.log" 2>/dev/null; then
	sed 's/^/#   /' "$TMP/vg.log"
	fail "valgrind がエラーを報告しました (exit $rc)"
fi

echo "# valgrind (memcheck): 起動〜管理〜終了でエラー・確実なリークなし（抑制は tools/valgrind.supp の 1 件のみ）"
exit 0
