#!/bin/sh
#
# check-brand.sh - SPEC §11.1: 名称に関わるリテラルは src/brand.h だけに置く、を検査する
#
# brand.h の WM_NAME マクロの値を「正解」として読み取り (ハードコードしない)、
# src/ 配下の brand.h 以外のファイルにそのリテラルが出現したら失敗する。
# これにより名称変更時は brand.h の書き換えだけで済むことを継続的に保証する。
#
# コメント除外について (このスクリプトの仕様):
#   /* ... */ と // ... のコメント内での言及は検査対象から除外する。
#   「(旧仮称 w98wm のこと)」のようにコメントで経緯を説明することまで
#   禁止すると、変更履歴やレビューコメントが書けなくなり本末転倒になるため。
#   検査したいのはあくまで「コード上のリテラル・識別子としての出現」であり、
#   人間向けの説明文ではない。
#   除外の実装は行単位の簡易な状態機械 (awk) で、文字列/文字リテラルの中身は
#   保持したままコメントだけを取り除く。複数行にまたがるブロックコメントの
#   状態は行をまたいで引き継ぐ。文字列リテラル内に "/*" 等が現れるような
#   病的なケース (このコードベースでは想定していない) までは正確に扱えない。
#
# もう1つの除外について: `#include "<literal>.h"` の行も検査対象から除外する。
#   src/w98wm.h のような「プロジェクト名 + .h」というモジュール内部の
#   ヘッダファイル名は、SPEC §11.1 が問題にしている「バイナリ名・設定パス・
#   WM_CLASS・ログ接頭辞・独自アトム名」等の対外的に見える名称とは別物であり、
#   これを対象に含めると #include するすべてのファイルが機械的に違反扱いに
#   なってしまい、チェックとして意味を成さなくなる。
#
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_DIR="$ROOT_DIR/src"
BRAND_H="$SRC_DIR/brand.h"

if [ ! -f "$BRAND_H" ]; then
	echo "エラー: $BRAND_H が見つかりません。" >&2
	exit 1
fi

LITERAL=$(sed -n 's/^#define[[:space:]]\+WM_NAME[[:space:]]\+"\([^"]*\)".*/\1/p' "$BRAND_H" | head -n1)

if [ -z "$LITERAL" ]; then
	echo "エラー: $BRAND_H から WM_NAME マクロの値を読み取れませんでした。" >&2
	echo "        '#define WM_NAME \"...\"' の書式を確認してください。" >&2
	exit 1
fi

echo "検査するリテラル (src/brand.h の WM_NAME より取得): \"$LITERAL\""

if [ ! -d "$SRC_DIR" ]; then
	echo "エラー: $SRC_DIR が見つかりません。" >&2
	exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

# コメント除去用 awk スクリプト。文字列/文字リテラルはそのまま残す。
cat >"$WORKDIR/strip_comments.awk" <<'AWK_EOF'
BEGIN { in_block = 0 }
{
	line = $0
	out = ""
	n = length(line)
	i = 1
	while (i <= n) {
		c  = substr(line, i, 1)
		c2 = substr(line, i, 2)
		if (in_block) {
			if (c2 == "*/") { in_block = 0; i += 2; continue }
			i++
			continue
		}
		if (c == "\"") {
			out = out c
			i++
			while (i <= n) {
				c = substr(line, i, 1)
				out = out c
				if (c == "\\") {
					i++
					if (i <= n) { out = out substr(line, i, 1); i++ }
					continue
				}
				i++
				if (c == "\"") break
			}
			continue
		}
		if (c == "'") {
			out = out c
			i++
			while (i <= n) {
				c = substr(line, i, 1)
				out = out c
				if (c == "\\") {
					i++
					if (i <= n) { out = out substr(line, i, 1); i++ }
					continue
				}
				i++
				if (c == "'") break
			}
			continue
		}
		if (c2 == "//") { break }
		if (c2 == "/*") { in_block = 1; i += 2; continue }
		out = out c
		i++
	}
	print out
}
AWK_EOF

HITS_FILE="$WORKDIR/hits"
: >"$HITS_FILE"

# ERE の特殊文字をエスケープした版 (#include フィルタの正規表現に使う)
LITERAL_RE=$(printf '%s' "$LITERAL" | sed 's/[][\.*^$/]/\\&/g')
INCLUDE_PATTERN="^[[:space:]]*#[[:space:]]*include[[:space:]]*\"${LITERAL_RE}\\.h\"[[:space:]]*\$"

# *.c / *.h のソースだけを対象にする (*.o 等のビルド成果物はバイナリであり、
# 文字列の偶然の一致を誤検出するだけなので対象外にする)。
find "$SRC_DIR" -type f \( -name '*.c' -o -name '*.h' \) ! -name 'brand.h' | while IFS= read -r f; do
	stripped=$(awk -f "$WORKDIR/strip_comments.awk" "$f")
	# 自分自身の名を冠したモジュール内部ヘッダ (#include "<literal>.h") は除外する
	filtered=$(printf '%s\n' "$stripped" | grep -vE "$INCLUDE_PATTERN") || true
	if printf '%s\n' "$filtered" | grep -qF -- "$LITERAL"; then
		printf '%s\n' "$f" >>"$HITS_FILE"
	fi
done

if [ -s "$HITS_FILE" ]; then
	echo ""
	echo "違反: 以下のファイルに \"$LITERAL\" のリテラルが (コメント外で) 見つかりました:"
	sed 's/^/  - /' "$HITS_FILE"
	echo ""
	echo "src/brand.h のマクロ経由で参照するように修正してください (SPEC §11.1)。"
	exit 1
fi

echo "OK: \"$LITERAL\" は src/brand.h 以外のコード上には出現していません。"
exit 0
