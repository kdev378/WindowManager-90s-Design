/*
 * ctext.c - COMPOUND_TEXT の復号 (SPEC §2.3.1)
 *
 * Xlib のアプリは、ロケールが UTF-8 以外のときや Xutf8 系を使わないとき、
 * WM_NAME を COMPOUND_TEXT 型で書く（例: ja_JP.UTF-8 での `xterm -T 日本語`）。
 * これを Latin-1 とみなすと ESC $ B ... の生バイトが化けた欧文として
 * タイトルに出る（外部テストで発見）。
 *
 * COMPOUND_TEXT は ISO 2022 の部分集合で、初期状態は
 *   GL = ASCII, GR = ISO 8859-1 の右半分
 * である。エスケープシーケンスで GL/GR の文字集合を切り替える。
 *
 * 対応する文字集合:
 *   ESC ( B / ESC ( J        GL ← ASCII / JIS X 0201 ローマ字
 *   ESC ( I / ESC ) I        GL/GR ← JIS X 0201 カタカナ (半角カナ)
 *   ESC - A                  GR ← ISO 8859-1 右半分
 *   ESC $ ( B / ESC $ B      GL ← JIS X 0208 (ESC $ @ の 1978 年版も同じ表で読む)
 *   ESC $ ) B                GR ← JIS X 0208
 *   ESC % G ... ESC % @      UTF-8 区間 (XFree86 拡張。Xlib が実際に出す)
 * それ以外の文字集合 (GB2312, KS C 5601, 8859-2 以降 …) の文字は '?' にする。
 * 化けた字を並べるよりは「読めない字がある」ことが分かる方がよい。
 *
 * 状態はこの関数の中だけで完結する（静的な状態を持たない）。
 */
#include <stddef.h>
#include <stdint.h>

#include "w98wm.h"

enum cset {
	CS_ASCII,      /* 94 文字集合。GL では ASCII そのもの */
	CS_ROMAN,      /* JIS X 0201 ローマ字 (0x5C=¥, 0x7E=‾) */
	CS_KANA,       /* JIS X 0201 カタカナ */
	CS_LATIN1,     /* 96 文字集合 ISO 8859-1 右半分 */
	CS_JIS0208,    /* 94^2 文字集合 */
	CS_UNKNOWN1,   /* 対応しない 1 バイト集合 */
	CS_UNKNOWN2    /* 対応しない 2 バイト集合 */
};

/* UTF-8 で 1 文字書く。入らなければ 0 を返して何も書かない */
static size_t put_utf8(char *dst, size_t pos, size_t dmax, uint32_t cp)
{
	size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;

	if (pos + need > dmax)
		return 0;
	if (need == 1) {
		dst[pos] = (char)cp;
	} else if (need == 2) {
		dst[pos]     = (char)(0xC0 | (cp >> 6));
		dst[pos + 1] = (char)(0x80 | (cp & 0x3F));
	} else {
		dst[pos]     = (char)(0xE0 | (cp >> 12));
		dst[pos + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[pos + 2] = (char)(0x80 | (cp & 0x3F));
	}
	return need;
}

/* 94/96 文字集合の 1 バイト (下位 7 bit) を UCS に。0 は「読めない」 */
static uint32_t decode1(enum cset cs, uint8_t b)
{
	b &= 0x7Fu;
	switch (cs) {
	case CS_ASCII:
		return b;
	case CS_ROMAN:
		if (b == 0x5C) return 0x00A5;
		if (b == 0x7E) return 0x203E;
		return b;
	case CS_KANA:
		if (b >= 0x21 && b <= 0x5F)
			return 0xFF61u + (uint32_t)(b - 0x21);
		return 0;
	case CS_LATIN1:
		return 0x80u | b;
	default:
		return 0;
	}
}

/* ESC の後ろの「中間バイト列 + 終端バイト」から指示先と文字集合を決める */
static void designate(const uint8_t *p, size_t n,
                      enum cset *gl, enum cset *gr)
{
	uint8_t i0 = n > 0 ? p[0] : 0;
	uint8_t i1 = n > 1 ? p[1] : 0;
	uint8_t f  = n > 0 ? p[n - 1] : 0;

	if (n == 2 && i0 == '(') {                 /* 94 → GL */
		*gl = f == 'B' ? CS_ASCII : f == 'J' ? CS_ROMAN :
		      f == 'I' ? CS_KANA  : CS_UNKNOWN1;
	} else if (n == 2 && i0 == ')') {          /* 94 → GR */
		*gr = f == 'B' ? CS_ASCII : f == 'J' ? CS_ROMAN :
		      f == 'I' ? CS_KANA  : CS_UNKNOWN1;
	} else if (n == 2 && i0 == '-') {          /* 96 → GR */
		*gr = f == 'A' ? CS_LATIN1 : CS_UNKNOWN1;
	} else if (n == 2 && i0 == '$') {          /* ESC $ F (旧形式) → GL */
		*gl = (f == 'B' || f == '@') ? CS_JIS0208 : CS_UNKNOWN2;
	} else if (n == 3 && i0 == '$' && i1 == '(') {
		*gl = (f == 'B' || f == '@') ? CS_JIS0208 : CS_UNKNOWN2;
	} else if (n == 3 && i0 == '$' && i1 == ')') {
		*gr = (f == 'B' || f == '@') ? CS_JIS0208 : CS_UNKNOWN2;
	}
	/* それ以外 (未知の指示) は状態を変えずに読み飛ばす */
}

size_t ctext_to_utf8(const uint8_t *src, size_t n, char *dst, size_t dmax)
{
	enum cset gl = CS_ASCII, gr = CS_LATIN1;
	bool utf8 = false;      /* ESC % G の区間 */
	size_t i = 0, out = 0;

	while (i < n) {
		uint8_t b = src[i];
		uint32_t cp;
		size_t w;

		if (b == 0x1B) {                         /* ESC */
			size_t j = i + 1;

			/* 中間バイト 0x20-0x2F の後に終端バイト 0x30-0x7E */
			while (j < n && src[j] >= 0x20 && src[j] <= 0x2F)
				j++;
			if (j >= n)
				break;                           /* 途中で切れている */
			if (j - (i + 1) == 1 && src[i + 1] == '%') {
				if (src[j] == 'G')
					utf8 = true;
				else if (src[j] == '@')
					utf8 = false;
			} else if (j - (i + 1) == 2 && src[i + 1] == '%' &&
			           src[i + 2] == '/') {
				/* 拡張区間 ESC % / F M L: 長さ (M-128)*128 + (L-128) を飛ばす */
				size_t len;

				if (j + 2 >= n)
					break;
				len = (size_t)((src[j + 1] & 0x7F) * 128 + (src[j + 2] & 0x7F));
				i = j + 3 + len;
				continue;
			} else if (!utf8) {
				designate(&src[i + 1], j - i, &gl, &gr);
			}
			i = j + 1;
			continue;
		}

		if (utf8) {
			/* UTF-8 区間はそのまま写す（後段の title_sanitize が検査する） */
			if (out + 1 > dmax)
				break;
			dst[out++] = (char)b;
			i++;
			continue;
		}

		if (b == 0x9B) {                         /* CSI: 方向指定。読み飛ばす */
			i++;
			while (i < n && !(src[i] >= 0x40 && src[i] <= 0x7E))
				i++;
			i++;
			continue;
		}

		if (b < 0x20 || (b >= 0x80 && b < 0xA0)) {
			/* HT と NL だけが許される制御文字。タイトルでは空白にする */
			if (b == 0x09 || b == 0x0A) {
				w = put_utf8(dst, out, dmax, ' ');
				if (w == 0)
					break;
				out += w;
			}
			i++;
			continue;
		}

		{
			enum cset cs = b < 0x80 ? gl : gr;

			if (b == 0x20 && cs != CS_LATIN1) {
				cp = ' ';                         /* GL の空白はどの集合でも空白 */
				i++;
			} else if (cs == CS_JIS0208 || cs == CS_UNKNOWN2) {
				if (i + 1 >= n)
					break;
				cp = cs == CS_JIS0208 ? jis0208_to_ucs(b, src[i + 1]) : 0;
				i += 2;
			} else {
				cp = decode1(cs, b);
				i++;
			}
		}
		w = put_utf8(dst, out, dmax, cp != 0 ? cp : '?');
		if (w == 0)
			break;
		out += w;
	}
	return out;
}
