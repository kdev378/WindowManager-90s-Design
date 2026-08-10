#!/bin/sh
#
# run-tests.sh - テストドライバ
#
# tests/unit/* (もし存在すれば) を先に実行し、続けて tests/integration/*.sh を
# 実行する。TAP 風 (厳密な TAP 仕様ではない) の出力をし、1件でも失敗があれば
# 非0で終了する (CI から呼べるように)。
#
# 結合テストの Xvfb/WM ライフサイクルについて:
#   各 tests/integration/*.sh は tests/integration/lib.sh を読み込み、
#   自分専用のフレッシュな Xvfb を起動し、その上で WM を起動する。
#   これは 040-saveset.sh が WM プロセスを kill -9 で落とすテストであり、
#   Xvfb/WM をテスト間で共有すると後続のテストが巻き添えで壊れるため。
#   このスクリプトはテストスクリプトを1本ずつ実行し結果を集計するだけで、
#   Xvfb/WM 自体の起動には関与しない。
#
# 各テストの終了コードの意味:
#   0   成功
#   77  スキップ (automake 由来の慣習。必要なツール/バイナリが無い場合)
#   その他  失敗
#
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UNIT_DIR="$ROOT_DIR/tests/unit"
INTEG_DIR="$ROOT_DIR/tests/integration"
export W98WM_BIN="$ROOT_DIR/w98wm"

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
	TIMEOUT_BIN="timeout 240"
fi

n=0
pass=0
fail=0
skip=0

#
# テストが取り残したサーバプロセスを落とす。
#
# lib.sh の trap も後始末をするが、それだけでは取りこぼしが出る
# （実測で 13 本 x 2 回の実行後に Xvfb が 6 個、w98wm が 4 個、
#   いずれも PPID=1 で孤児化した状態で残っていた）。
# CI で繰り返すと積み上がって最終的にメモリを食い潰すので、
# ランナ側でも網を張る。
#
# 対象は **Xvfb と w98wm だけ**にする。xterm まで落とすと、
# 同時に走っているかもしれない memcheck --long の被験ウィンドウを
# 巻き添えにする（Xvfb を落とせばぶら下がる xterm も接続断で終わる）。
#
#
# ★ 「前後のスナップショットの差」で回収してはいけない。
#
#   最初はそう書いたが、**このランナを 2 つ同時に走らせると
#   互いの Xvfb と w98wm を「リークした」と誤認して殺し合う**。
#   実際に踏んだ（6 本が「WM が起動直後に終了しました」で落ちた）。
#
#   代わりに、このランナが起動したプロセスに環境変数で印を付け、
#   **自分の印を持つものだけ**を回収する。差分を取る必要も無くなる
#   （テストが終わった時点で自分の印を持つサーバは全部リークなので）。
#
#   /proc が読めない環境では回収を諦める（lib.sh の trap に任せる）。
#
W98WM_TEST_RUN="run-$$-$(date +%s)"
export W98WM_TEST_RUN

_reap_leaked_servers() {
	[ -d /proc ] || return 0

	leaked=""
	for p in $( { pgrep -x Xvfb; pgrep -x w98wm; } 2>/dev/null | sort -u); do
		[ -r "/proc/$p/environ" ] || continue
		# 読む間に消えることがある。エラーは無視してよい
		if tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null |
				grep -qxF "W98WM_TEST_RUN=$W98WM_TEST_RUN" 2>/dev/null; then
			leaked="$leaked $p"
		fi
	done
	[ -z "$leaked" ] && return 0

	for p in $leaked; do
		kill "$p" 2>/dev/null || true
	done
	sleep 0.3
	for p in $leaked; do
		kill -9 "$p" 2>/dev/null || true
	done
}

run_one() {
	path="$1"
	name=$(basename "$path")
	n=$((n + 1))
	logf=$(mktemp)

	if [ -n "$TIMEOUT_BIN" ]; then
		$TIMEOUT_BIN "$path" >"$logf" 2>&1 && rc=0 || rc=$?
	else
		"$path" >"$logf" 2>&1 && rc=0 || rc=$?
	fi

	_reap_leaked_servers

	if [ "$rc" -eq 0 ]; then
		echo "ok $n - $name"
		pass=$((pass + 1))
	elif [ "$rc" -eq 77 ]; then
		echo "ok $n - $name # SKIP"
		skip=$((skip + 1))
	elif [ "$rc" -eq 124 ] && [ -n "$TIMEOUT_BIN" ]; then
		echo "not ok $n - $name (タイムアウト)"
		sed 's/^/#   /' "$logf"
		fail=$((fail + 1))
	else
		echo "not ok $n - $name (exit $rc)"
		sed 's/^/#   /' "$logf"
		fail=$((fail + 1))
	fi
	rm -f "$logf"
}

# --- テスト総数を事前に数えて TAP 風の予告行を出す ---
count=0
if [ -d "$UNIT_DIR" ]; then
	for t in "$UNIT_DIR"/*; do
		[ -f "$t" ] || continue
		[ -x "$t" ] || continue
		count=$((count + 1))
	done
fi
if [ -d "$INTEG_DIR" ]; then
	for t in "$INTEG_DIR"/*.sh; do
		[ -e "$t" ] || continue
		[ "$(basename "$t")" = "lib.sh" ] && continue
		count=$((count + 1))
	done
fi
echo "1..$count"

# --- 単体テスト (X 不要) ---
if [ -d "$UNIT_DIR" ]; then
	for t in "$UNIT_DIR"/*; do
		[ -f "$t" ] || continue
		if [ ! -x "$t" ]; then
			echo "# skip (実行不可): $t"
			continue
		fi
		run_one "$t"
	done
else
	echo "# tests/unit が無いためスキップします"
fi

# --- 結合テスト (Xvfb + WM) ---
if [ -d "$INTEG_DIR" ]; then
	for t in "$INTEG_DIR"/*.sh; do
		[ -e "$t" ] || continue
		[ "$(basename "$t")" = "lib.sh" ] && continue
		run_one "$t"
	done
else
	echo "# tests/integration が無いためスキップします"
fi

echo "# --- 集計: 合計 $n / 成功 $pass / 失敗 $fail / スキップ $skip ---"

if [ "$fail" -gt 0 ]; then
	exit 1
fi
exit 0
