#!/bin/sh
#
# check-supported.sh - SPEC §5.2.1: _NET_SUPPORTED に載る各アトムに
# 対応する結合テストが存在することを機械的に検査する。
#
# src/atoms.h の ATOM_LIST という X-macro を解析し、第2引数が ATOM_SUP の
# エントリ (= _NET_SUPPORTED に載るアトム) それぞれについて、
# tests/integration/ 配下のいずれかのファイルがそのアトム文字列に
# 言及しているかどうかを grep で確認する。
#
# SPEC §5.2.1 の通り、このチェックは「軽量さ」を優先する。テストの質は問わず、
# 存在するかどうかだけを機械的に見る (xdotool/xprop を数行叩くだけのテストで良い)。
#
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ATOMS_H="$ROOT_DIR/src/atoms.h"
TEST_DIR="$ROOT_DIR/tests/integration"

if [ ! -f "$ATOMS_H" ]; then
	echo "エラー: $ATOMS_H が見つかりません。" >&2
	exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

# X(識別子, ATOM_SUP|ATOM_KNOWN, "文字列") の行を "識別子 状態 文字列" の3列に変換する。
grep -E '^[[:space:]]*X\(' "$ATOMS_H" |
	sed -E 's/^[[:space:]]*X\(([A-Za-z0-9_]+),[[:space:]]*(ATOM_SUP|ATOM_KNOWN),[[:space:]]*"([^"]*)"\).*/\1 \2 \3/' \
		>"$WORKDIR/parsed.txt"

if [ ! -s "$WORKDIR/parsed.txt" ]; then
	echo "エラー: $ATOMS_H から ATOM_LIST のエントリを1件も解析できませんでした。" >&2
	echo "        X-macro の書式が変わっていないか確認してください。" >&2
	exit 1
fi

awk '$2 == "ATOM_SUP" { print $3 }' "$WORKDIR/parsed.txt" >"$WORKDIR/sup_atoms.txt"

if [ ! -s "$WORKDIR/sup_atoms.txt" ]; then
	echo "警告: ATOM_SUP のエントリが1件もありません (_NET_SUPPORTED は空になります)。"
	exit 0
fi

if [ ! -d "$TEST_DIR" ]; then
	echo "エラー: $TEST_DIR が見つかりません。ATOM_SUP のアトムに対応する結合テストがありません。" >&2
	total=$(wc -l <"$WORKDIR/sup_atoms.txt" | tr -d ' ')
	echo "検査対象: $total アトム, 未対応: $total" >&2
	exit 1
fi

MISSING_FILE="$WORKDIR/missing.txt"
: >"$MISSING_FILE"
total=0

while IFS= read -r atom; do
	[ -z "$atom" ] && continue
	total=$((total + 1))
	if grep -rlF -- "$atom" "$TEST_DIR" >/dev/null 2>&1; then
		: # OK: どこかのテストファイルがこのアトムに言及している
	else
		echo "$atom" >>"$MISSING_FILE"
	fi
done <"$WORKDIR/sup_atoms.txt"

missing=$(wc -l <"$MISSING_FILE" | tr -d ' ')

echo "検査対象 (_NET_SUPPORTED / ATOM_SUP): $total アトム"

if [ "$missing" -gt 0 ]; then
	echo ""
	echo "以下のアトムは ATOM_SUP (_NET_SUPPORTED に載る) ですが、"
	echo "tests/integration/ 配下のどのファイルにも言及がありません:"
	sed 's/^/  - /' "$MISSING_FILE"
	echo ""
	echo "SPEC §5.2.1: テストの無いアトムを _NET_SUPPORTED に載せてはいけません。"
	echo "対応する軽量な結合テスト (xdotool/xprop 数行で良い) を追加するか、"
	echo "実装が終わるまで ATOM_KNOWN に留めてください。"
	echo ""
	echo "未対応: $missing / $total"
	exit 1
fi

echo "未対応: 0 / $total"
echo "OK: ATOM_SUP の全アトムに対応する結合テストの言及があります。"
exit 0
