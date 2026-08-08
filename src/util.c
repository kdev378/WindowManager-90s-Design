/*
 * util.c - ログ、文字列、スラブアロケータ、幾何、ヒント計算 (SPEC §2.3.0, §2.3.1)
 *
 * 他の .c から include されない。宣言は w98wm.h / compat.h にある。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "w98wm.h"

/* ================================================================== *
 * ログ (SPEC §2.2.1 相当。-v の時だけ log_msg を出す。log_err は常に出す)
 * ================================================================== */

static int g_verbose;

void log_init(int verbose)
{
	g_verbose = (verbose != 0);
}

void log_msg(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;

	fprintf(stderr, WM_NAME ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

void log_err(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, WM_NAME ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/* ================================================================== *
 * strlcpy / strlcat (compat.h が「無い環境」向けとして要求する実装)
 *   OpenBSD 版と同じ意味論。dsize==0 でも dst に書き込まない。
 * ================================================================== */

#ifndef WM_HAVE_STRLCPY

size_t wm_strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc = src;
	size_t nleft = dsize;

	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}

	if (nleft == 0) {
		if (dsize != 0)
			*dst = '\0';
		while (*src++ != '\0')
			;
	}

	return (size_t)(src - osrc - 1);
}

size_t wm_strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dsize;
	size_t dlen;

	while (n != 0 && *dst != '\0') {
		dst++;
		n--;
	}
	dlen = (size_t)(dst - odst);
	n = dsize - dlen;

	if (n == 0)
		return dlen + strlen(src);

	while (*src != '\0') {
		if (n != 1) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return dlen + (size_t)(src - osrc);
}

#endif /* !WM_HAVE_STRLCPY */

/* ================================================================== *
 * 単調時刻 (compat.h 契約)
 * ================================================================== */

uint64_t wm_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ================================================================== *
 * スラブアロケータ (SPEC §2.3.0)
 *
 *   チャンク 0 は BSS の静的配列（malloc しない）。
 *   65 個目以降はチャンク単位（WM_SLAB_CHUNK 個）で malloc し、連結する。
 *   上限 WM_MAX_CLIENTS を超える、または malloc 失敗時は NULL を返す。
 *   呼び出し側（client.c）はこれを見て「装飾なしの非管理ウィンドウ」に
 *   縮退させる（§2.3.0）。ここでは失敗を通知するだけでよい。
 * ================================================================== */

struct slab_chunk {
	struct client      clients[WM_SLAB_CHUNK];
	struct slab_chunk *next;
};

static struct client bss_chunk0[WM_SLAB_CHUNK];   /* チャンク 0: BSS (§2.3.0) */
static struct slab_chunk *slab_tail;              /* 直近に確保したヒープチャンク */
static uint16_t slab_capacity = WM_SLAB_CHUNK;     /* 確保済みの総スロット数 */
static uint16_t slab_carved;                       /* 一度でも払い出したスロット数 */
static struct client *slab_free_list;              /* 解放済みスロットの連結リスト */
static uint16_t slab_live;                         /* 現在使用中の数 */

struct client *slab_alloc(void)
{
	struct client *c;

	if (slab_free_list != NULL) {
		c = slab_free_list;
		slab_free_list = c->next;
	} else {
		uint16_t slot;

		if (slab_carved >= WM_MAX_CLIENTS)
			return NULL;   /* 管理数上限 (§2.3.0) */

		if (slab_carved >= slab_capacity) {
			struct slab_chunk *nc = malloc(sizeof(*nc));

			if (nc == NULL)
				return NULL;   /* 確保失敗 (§2.3.0) */
			nc->next = NULL;
			if (slab_tail != NULL)
				slab_tail->next = nc;
			slab_tail = nc;
			slab_capacity += WM_SLAB_CHUNK;
		}

		slot = (uint16_t)(slab_carved % WM_SLAB_CHUNK);
		c = (slab_carved < WM_SLAB_CHUNK) ? &bss_chunk0[slot]
		                                  : &slab_tail->clients[slot];
		slab_carved++;
	}

	memset(c, 0, sizeof(*c));   /* 「確保時にゼロクリア」契約 */
	slab_live++;
	return c;
}

void slab_free(struct client *c)
{
	if (c == NULL)
		return;

	c->next = slab_free_list;
	slab_free_list = c;
	if (slab_live > 0)
		slab_live--;
}

uint16_t slab_count(void)
{
	return slab_live;
}

/* ================================================================== *
 * 幾何 (SPEC §3.5 モニタ選択のフォールバックなどに使う)
 * ================================================================== */

uint32_t rect_overlap_area(const struct rect *a, const struct rect *b)
{
	int32_t ax1 = a->x, ay1 = a->y;
	int32_t ax2 = ax1 + (int32_t)a->w;
	int32_t ay2 = ay1 + (int32_t)a->h;
	int32_t bx1 = b->x, by1 = b->y;
	int32_t bx2 = bx1 + (int32_t)b->w;
	int32_t by2 = by1 + (int32_t)b->h;

	int32_t ix1 = (ax1 > bx1) ? ax1 : bx1;
	int32_t iy1 = (ay1 > by1) ? ay1 : by1;
	int32_t ix2 = (ax2 < bx2) ? ax2 : bx2;
	int32_t iy2 = (ay2 < by2) ? ay2 : by2;

	if (ix2 <= ix1 || iy2 <= iy1)
		return 0;

	return (uint32_t)(ix2 - ix1) * (uint32_t)(iy2 - iy1);
}

bool rect_contains_point(const struct rect *r, int16_t x, int16_t y)
{
	return x >= r->x && x < r->x + r->w &&
	       y >= r->y && y < r->y + r->h;
}

void rect_center(const struct rect *r, int16_t *cx, int16_t *cy)
{
	*cx = (int16_t)(r->x + (int16_t)(r->w / 2));
	*cy = (int16_t)(r->y + (int16_t)(r->h / 2));
}

/* ================================================================== *
 * WM_NORMAL_HINTS の適用 (ICCCM §4.1.2.3)
 *
 *   1. min/max サイズを適用
 *   2. リサイズ増分をベースサイズ基準で適用
 *      （ベースサイズが無ければ最小サイズを、最小サイズが無ければ
 *        ベースサイズを、どちらも無ければ 1x1 を使う — ICCCM の規定通り）
 *   3. アスペクト比制約を適用
 *   すべて 0 除算・オーバーフローを避けて計算する。
 * ================================================================== */

void hints_apply(const struct size_hints *h, uint16_t *w, uint16_t *h_out)
{
	uint16_t min_w, min_h, max_w, max_h;
	uint16_t base_w, base_h;
	uint16_t cw, ch;

	if (h == NULL || w == NULL || h_out == NULL)
		return;

	cw = *w;
	ch = *h_out;
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;

	/* ベースサイズと最小サイズは互いのデフォルト (ICCCM §4.1.2.3) */
	if (h->flags & HINT_BASE_SIZE) {
		base_w = h->base_w;
		base_h = h->base_h;
	} else if (h->flags & HINT_MIN_SIZE) {
		base_w = h->min_w;
		base_h = h->min_h;
	} else {
		base_w = 1;
		base_h = 1;
	}

	if (h->flags & HINT_MIN_SIZE) {
		min_w = h->min_w;
		min_h = h->min_h;
	} else if (h->flags & HINT_BASE_SIZE) {
		min_w = h->base_w;
		min_h = h->base_h;
	} else {
		min_w = 1;
		min_h = 1;
	}
	if (min_w < 1)
		min_w = 1;
	if (min_h < 1)
		min_h = 1;

	if (h->flags & HINT_MAX_SIZE) {
		max_w = h->max_w;
		max_h = h->max_h;
		if (max_w < min_w)
			max_w = min_w;
		if (max_h < min_h)
			max_h = min_h;
	} else {
		max_w = UINT16_MAX;
		max_h = UINT16_MAX;
	}

	/* 1. min/max */
	if (cw < min_w)
		cw = min_w;
	if (ch < min_h)
		ch = min_h;
	if (cw > max_w)
		cw = max_w;
	if (ch > max_h)
		ch = max_h;

	/* 2. リサイズ増分（ベースサイズ基準） */
	if ((h->flags & HINT_RESIZE_INC) != 0) {
		if (h->inc_w > 0) {
			if (cw > base_w) {
				uint32_t d = (uint32_t)(cw - base_w);
				cw = (uint16_t)(base_w + (d / h->inc_w) * h->inc_w);
			} else {
				cw = base_w;
			}
		}
		if (h->inc_h > 0) {
			if (ch > base_h) {
				uint32_t d = (uint32_t)(ch - base_h);
				ch = (uint16_t)(base_h + (d / h->inc_h) * h->inc_h);
			} else {
				ch = base_h;
			}
		}
		if (cw < min_w)
			cw = min_w;
		if (ch < min_h)
			ch = min_h;
		if (cw > max_w)
			cw = max_w;
		if (ch > max_h)
			ch = max_h;
	}

	/* 3. アスペクト比（分母 0 を必ず避ける） */
	if ((h->flags & HINT_ASPECT) != 0) {
		if (h->min_aspect_num > 0 && h->min_aspect_den > 0) {
			/* cw/ch < min_num/min_den  <=>  cw*min_den < min_num*ch */
			int64_t lhs = (int64_t)cw * h->min_aspect_den;
			int64_t rhs = (int64_t)h->min_aspect_num * ch;

			if (lhs < rhs) {
				int64_t new_h = ((int64_t)cw * h->min_aspect_den) /
				                 h->min_aspect_num;
				if (new_h < 1)
					new_h = 1;
				if (new_h <= UINT16_MAX)
					ch = (uint16_t)new_h;
			}
		}
		if (h->max_aspect_num > 0 && h->max_aspect_den > 0) {
			/* cw/ch > max_num/max_den  <=>  cw*max_den > max_num*ch */
			int64_t lhs = (int64_t)cw * h->max_aspect_den;
			int64_t rhs = (int64_t)h->max_aspect_num * ch;

			if (lhs > rhs) {
				int64_t new_h = ((int64_t)cw * h->max_aspect_den) /
				                 h->max_aspect_num;
				if (new_h < 1)
					new_h = 1;
				if (new_h <= UINT16_MAX)
					ch = (uint16_t)new_h;
			}
		}
		if (cw < min_w)
			cw = min_w;
		if (ch < min_h)
			ch = min_h;
		if (cw > max_w)
			cw = max_w;
		if (ch > max_h)
			ch = max_h;
	}

	*w = cw;
	*h_out = ch;
}

/* ================================================================== *
 * UTF-8 → UCS-2 変換 + サニタイズ (SPEC §2.3.1)
 * ================================================================== */

size_t utf8_to_ucs2(const char *src, size_t src_len, uint16_t *out, size_t out_max)
{
	size_t i = 0;
	size_t n = 0;
	/* 各シーケンス長ごとのオーバーロング判定下限 (index = seqlen) */
	static const uint32_t overlong_min[5] = { 0, 0, 0x80, 0x800, 0x10000 };

	if (src == NULL || out == NULL || out_max == 0)
		return 0;

	while (i < src_len && n < out_max) {
		unsigned char c0 = (unsigned char)src[i];
		uint32_t cp;
		size_t seqlen;

		if (c0 < 0x80) {
			cp = c0;
			seqlen = 1;
		} else if ((c0 & 0xE0) == 0xC0) {
			cp = (uint32_t)(c0 & 0x1F);
			seqlen = 2;
		} else if ((c0 & 0xF0) == 0xE0) {
			cp = (uint32_t)(c0 & 0x0F);
			seqlen = 3;
		} else if ((c0 & 0xF8) == 0xF0) {
			cp = (uint32_t)(c0 & 0x07);
			seqlen = 4;
		} else {
			/* 先頭バイトとして不正（継続バイト単独 or 0xF8-0xFF） */
			out[n++] = 0xFFFDu;
			i += 1;
			continue;
		}

		if (i + seqlen > src_len) {
			/* 末尾で切れている */
			out[n++] = 0xFFFDu;
			i += 1;
			continue;
		}

		{
			size_t j;
			bool ok = true;

			for (j = 1; j < seqlen; j++) {
				unsigned char cc = (unsigned char)src[i + j];

				if ((cc & 0xC0) != 0x80) {
					ok = false;
					break;
				}
				cp = (cp << 6) | (uint32_t)(cc & 0x3F);
			}
			if (!ok) {
				out[n++] = 0xFFFDu;
				i += 1;
				continue;
			}
		}

		/* オーバーロング・サロゲート・レンジ外は不正 (§2.3.1) */
		if (cp < overlong_min[seqlen] ||
		    (cp >= 0xD800u && cp <= 0xDFFFu) ||
		    cp > 0x10FFFFu) {
			out[n++] = 0xFFFDu;
			i += 1;
			continue;
		}

		i += seqlen;

		/* C0/C1 制御文字・双方向オーバーライドは除去 (§2.3.1) */
		if (cp <= 0x1Fu || cp == 0x7Fu ||
		    (cp >= 0x80u && cp <= 0x9Fu) ||
		    (cp >= 0x202Au && cp <= 0x202Eu) ||
		    (cp >= 0x2066u && cp <= 0x2069u)) {
			continue;
		}

		if (cp > 0xFFFFu)
			out[n++] = 0xFFFDu;   /* BMP 外はサロゲート分解せず置換 (§2.3.1) */
		else
			out[n++] = (uint16_t)cp;
	}

	return n;
}

/* UTF-8 のコードポイント境界で安全に切り詰めた長さを返す (§2.3.1) */
size_t utf8_truncate_len(const char *s, size_t len, size_t max)
{
	size_t limit = (len < max) ? len : max;
	size_t i;
	unsigned char lead;
	size_t seqlen;

	if (s == NULL || limit == 0)
		return 0;

	i = limit;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
		i--;

	if (i == 0)
		return 0;   /* 継続バイトしか無い（壊れている） */

	lead = (unsigned char)s[i - 1];
	if (lead < 0x80)
		seqlen = 1;
	else if ((lead & 0xE0) == 0xC0)
		seqlen = 2;
	else if ((lead & 0xF0) == 0xE0)
		seqlen = 3;
	else if ((lead & 0xF8) == 0xF0)
		seqlen = 4;
	else
		seqlen = 1;   /* 不正なリードバイトは単独の 1 バイトとして扱う */

	if ((i - 1) + seqlen > limit)
		return i - 1;   /* 末尾のシーケンスが途中で切れる → 丸ごと落とす */

	return limit;
}
