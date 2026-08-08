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
	TIMEOUT_BIN="timeout 90"
fi

n=0
pass=0
fail=0
skip=0

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
