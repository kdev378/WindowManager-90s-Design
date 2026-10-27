#!/bin/sh
#
# 020-cli-args.sh — コマンドライン引数の扱い (X 不要)
#
# 外部テスト（2026-10-27）で、未知の引数が黙って無視されていた。
# `--replase` のような打ち間違いで「既に WM が動いている」という
# 別のエラーに化ける。未知の引数は usage を出して非 0 で終わること。
#
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="${W98WM_BIN:-$ROOT/w98wm}"
[ -x "$BIN" ] || { echo "w98wm がビルドされていません"; exit 77; }

# X に繋がらないようにしておく。引数の検査は接続より前に終わるはず
unset DISPLAY

out=$("$BIN" --replase 2>&1) && rc=0 || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL: 未知の引数で rc=$rc (期待 2)"; exit 1; }
echo "$out" | grep -q -- "--replase" || { echo "FAIL: どの引数が悪いか出ていない: $out"; exit 1; }
echo "$out" | grep -q "usage" || { echo "FAIL: usage が出ていない: $out"; exit 1; }

out=$("$BIN" -v bogus 2>&1) && rc=0 || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL: 既知 + 未知の組み合わせで rc=$rc"; exit 1; }

"$BIN" --help >/dev/null 2>&1 || { echo "FAIL: --help が非 0"; exit 1; }
"$BIN" -h >/dev/null 2>&1 || { echo "FAIL: -h が非 0"; exit 1; }
"$BIN" --version | grep -q "[0-9]" || { echo "FAIL: --version"; exit 1; }

echo "ok"
