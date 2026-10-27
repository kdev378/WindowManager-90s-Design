#!/bin/sh
#
# 010-text-codec.sh — COMPOUND_TEXT の復号と JIS X 0208 対応表 (X 不要)
#
# 外部テスト（2026-10-27）で、`xterm -T 日本語タイトル` の WM_NAME
# （COMPOUND_TEXT 型 = ISO 2022 の ESC $ B ...）が Latin-1 として読まれ、
# 化けた欧文で表示されていた。その回帰テスト。
#
# 期待値は Python の codecs ではなく**手で書いた**バイト列と照合する
# （対応表を同じ codecs から生成しているので、codecs と比べても
#   生成器のバグは見つからない）。
#
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

command -v cc >/dev/null 2>&1 || { echo "cc がありません"; exit 77; }
pkg-config --exists xcb 2>/dev/null || { echo "xcb がありません"; exit 77; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

cat >"$TMP/t.c" <<'CEOF'
#include <stdio.h>
#include <string.h>
#include "w98wm.h"

static int fails;

static void check(const char *what, const char *in, size_t n, const char *want)
{
	char out[256];
	size_t len = ctext_to_utf8((const uint8_t *)in, n, out, sizeof out - 1);

	out[len] = '\0';
	if (strcmp(out, want) != 0) {
		printf("FAIL %s: got [%s] want [%s]\n", what, out, want);
		fails++;
	}
}

#define CHECK(what, lit, want) check(what, lit, sizeof(lit) - 1, want)

int main(void)
{
	/* 既定状態: GL=ASCII, GR=Latin-1 */
	CHECK("ascii", "xterm", "xterm");
	CHECK("latin1-gr", "caf\xe9", "caf\xc3\xa9");

	/* ESC $ ( B 日本語 ESC ( B  — JIS で「日本語」は 467C 4B5C 386C */
	CHECK("jis-gl", "\x1b$(B\x46\x7c\x4b\x5c\x38\x6c\x1b(B",
	      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
	/* 旧形式 ESC $ B */
	CHECK("jis-gl-old", "\x1b$B\x46\x7c\x1b(Bx", "\xe6\x97\xa5x");
	/* GR に JIS X 0208（EUC 風）: ESC $ ) B + 0xC6FC = 日 */
	CHECK("jis-gr", "\x1b$)B\xc6\xfc", "\xe6\x97\xa5");
	/* xterm -T 日本語タイトル が実際に書く形（ASCII 混在） */
	CHECK("xterm-mix",
	      "a\x1b$(B\x46\x7c\x4b\x5c\x38\x6c\x25\x3f\x25\x24\x25\x48\x25\x6b\x1b(Bz",
	      "a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
	      "\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x88\xe3\x83\xab" "z");
	/* 半角カナ ESC ) I + 0xB1 = ｱ (U+FF71) */
	CHECK("kana-gr", "\x1b)I\xb1", "\xef\xbd\xb1");
	/* JIS X 0201 ローマ字: 0x5C は ¥ */
	CHECK("roman", "\x1b(J\x5c", "\xc2\xa5");
	/* UTF-8 区間 ESC % G ... ESC % @ */
	CHECK("utf8-seg", "\x1b%G\xe6\x97\xa5\x1b%@!", "\xe6\x97\xa5!");
	/* 対応しない 2 バイト集合 (GB2312 = ESC $ ( A) は 1 文字 1 個の '?' */
	CHECK("unknown94n", "\x1b$(A\x30\x21\x30\x22\x1b(Bok", "??ok");
	/* 途中で切れたエスケープ・2 バイト目の欠落で暴走しない */
	CHECK("trunc-esc", "ab\x1b$(", "ab");
	CHECK("trunc-2byte", "\x1b$(B\x46", "");
	/* CSI の方向指定は読み飛ばす */
	CHECK("csi", "\x9b" "1]ab\x9b]", "ab");
	/* 出力バッファが足りないとき、文字の途中で切らない */
	{
		char out[4];
		const char in[] = "\x1b$(B\x46\x7c\x4b\x5c";
		size_t len = ctext_to_utf8((const uint8_t *)in, sizeof in - 1, out, 4);
		if (len != 3) {
			printf("FAIL short-buffer: len=%zu want 3\n", len);
			fails++;
		}
	}

	/* 対応表の往復と異体 */
	if (jis0208_to_ucs(0x46, 0x7c) != 0x65E5) { printf("FAIL to_ucs 日\n"); fails++; }
	if (ucs_to_jis0208(0x65E5) != 0x467C)     { printf("FAIL to_jis 日\n"); fails++; }
	if (ucs_to_jis0208(0x30B9) != 0x2539)     { printf("FAIL to_jis ス\n"); fails++; }
	if (ucs_to_jis0208(0xFF5E) != 0x2141)     { printf("FAIL alias ～\n"); fails++; }
	if (ucs_to_jis0208('A') != 0)             { printf("FAIL ascii は 0\n"); fails++; }
	if (ucs_to_jis0208(0x4E02) != 0)          { printf("FAIL 表外は 0\n"); fails++; }
	{
		/* 全区点で往復が一致すること（生成器の取り違えを検出） */
		unsigned b1, b2, n = 0, bad = 0;
		for (b1 = 0x21; b1 <= 0x7e; b1++)
			for (b2 = 0x21; b2 <= 0x7e; b2++) {
				uint16_t u = jis0208_to_ucs((uint8_t)b1, (uint8_t)b2);
				uint16_t j;
				if (u == 0)
					continue;
				n++;
				j = ucs_to_jis0208(u);
				if (jis0208_to_ucs((uint8_t)(j >> 8), (uint8_t)j) != u)
					bad++;
			}
		if (n < 6800 || bad != 0) {
			printf("FAIL roundtrip n=%u bad=%u\n", n, bad);
			fails++;
		}
	}

	if (fails == 0)
		printf("ok\n");
	return fails == 0 ? 0 : 1;
}
CEOF

cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -I"$ROOT/src" \
	$(pkg-config --cflags xcb) -o "$TMP/t" "$TMP/t.c" \
	"$ROOT/src/ctext.c" "$ROOT/src/jis0208.c" 2>"$TMP/cc.log" || {
	cat "$TMP/cc.log"
	exit 1
}
"$TMP/t"
