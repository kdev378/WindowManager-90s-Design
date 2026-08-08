#!/bin/sh
#
# pixdiff.sh - スクリーンショットと参照画像のピクセル差分を報告する (PLAN.md Phase 2)
#
# 重要な注意 (SPEC §4.0):
#   「ピクセル精度で再現する」の対象は §4.0 が定める範囲に限られる。
#   一致を要求するのはフレーム・ベベル・キャプションバー・キャプションボタン・
#   メニュー・タスクバーの「幾何と色」であって、フォントのグリフ形状
#   (MS Sans Serif を同梱できない) やそれに伴う文字位置の微差は対象外。
#   したがって本スクリプトが報告する差分ピクセル数がゼロでないからといって
#   ただちに不合格ではない。差分画像 (--out) を実際に見て、差がタイトル文字
#   の字形だけに起因するのか、ベベル/色/幾何のズレなのかを目視で判断すること。
#   自動の合否判定はしない (数値と画像を出すだけ)。
#
# 使い方:
#   tools/pixdiff.sh --ref REF.png --in IN.png [--out DIFF.png]
#     --ref PATH   基準画像 (docs/SPEC.md §4.0 の tests/reference/ 相当)
#     --in  PATH   比較対象 (tools/screenshot.sh の出力など)
#     --out PATH   差分画像の出力先 (既定: --in と同じディレクトリの *.diff.png)
#     -h, --help   このヘルプを表示
#
# 終了コード:
#   0    比較を実行できた (差分ゼロ・非ゼロを問わない。件数は標準出力に出す)
#   1    実際のエラー (ファイルが無い・サイズが読めない等)
#   2    引数エラー
#   77   ImageMagick (compare) が無く、比較そのものをスキップした
#
# シェル: POSIX sh (dash で動作確認済み)。
#
set -eu

REF=""
IN=""
OUT=""

usage() {
	sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
	--ref)
		REF="$2"
		shift 2
		;;
	--in)
		IN="$2"
		shift 2
		;;
	--out)
		OUT="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "不明なオプション: $1" >&2
		exit 2
		;;
	esac
done

if [ -z "$REF" ] || [ -z "$IN" ]; then
	echo "エラー: --ref と --in は必須です。" >&2
	usage >&2
	exit 2
fi

if [ -z "$OUT" ]; then
	case "$IN" in
	*.png) OUT="${IN%.png}.diff.png" ;;
	*) OUT="${IN}.diff.png" ;;
	esac
fi

if ! command -v compare >/dev/null 2>&1; then
	echo "SKIP: ImageMagick の compare が見つかりません。比較をスキップします。" >&2
	exit 77
fi

if [ ! -f "$REF" ]; then
	echo "エラー: 参照画像が見つかりません: $REF" >&2
	exit 1
fi
if [ ! -f "$IN" ]; then
	echo "エラー: 比較対象画像が見つかりません: $IN" >&2
	exit 1
fi

if command -v identify >/dev/null 2>&1; then
	ref_size=$(identify -format '%wx%h' "$REF" 2>/dev/null || true)
	in_size=$(identify -format '%wx%h' "$IN" 2>/dev/null || true)
	if [ -n "$ref_size" ] && [ -n "$in_size" ] && [ "$ref_size" != "$in_size" ]; then
		echo "注記: 画像サイズが異なります (参照: $ref_size / 対象: $in_size)。" >&2
		echo "      compare は重なる領域だけを比較するため、この差はサイズ差そのものに" >&2
		echo "      起因する可能性が高い (ウィンドウ配置やジオメトリの確認を推奨)。" >&2
	fi
fi

OUT_DIR=$(dirname -- "$OUT")
mkdir -p "$OUT_DIR"

AE_LOG=$(mktemp)
trap 'rm -f "$AE_LOG"' EXIT

# compare -metric AE: 完全一致なら終了コード 0、差分ありなら 1、
# 画像が読めない等の実エラーなら 2 以上。差分ピクセル数は stderr に出る。
set +e
compare -metric AE "$REF" "$IN" "$OUT" 2>"$AE_LOG"
rc=$?
set -e

if [ "$rc" -ge 2 ]; then
	echo "エラー: compare の実行に失敗しました:" >&2
	cat "$AE_LOG" >&2
	exit 1
fi

AE=$(tail -n1 "$AE_LOG" | tr -d '[:space:]')
case "$AE" in
'' | *[!0-9]*)
	echo "エラー: 差分ピクセル数を読み取れませんでした (compare の出力: $(cat "$AE_LOG"))" >&2
	exit 1
	;;
esac

echo "差分ピクセル数 (AE): $AE"
echo "差分画像: $OUT"
if [ "$AE" -eq 0 ]; then
	echo "OK: 完全一致 (差分ピクセル 0)"
else
	echo "注記: 差分あり。SPEC §4.0 によりフォントのグリフ形状は比較対象外なので、"
	echo "      $OUT を目視して差の原因がベベル/色/幾何かグリフかを確認すること。"
fi

exit 0
