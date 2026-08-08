/*
 * cursor.c - マウスカーソル (SPEC §4.7)
 *
 * X の cursor フォント (XC_left_ptr 等) は X11 伝統の見た目で Win98 の意匠と
 * 喧嘩するため使わない。1bit の Pixmap 2 枚 (ソース+マスク) を自前で組み立て、
 * コアプロトコルの xcb_create_pixmap + xcb_put_image + xcb_create_cursor だけで
 * 形状を作る (§4.7)。
 *
 * ビットマップの持ち方について：
 *   各カーソルの形状は本体・ヘッダとも「幅×高さ の座標を受け取り、その画素が
 *   透明/白(背景)/黒(前景) のどれかを返す」小さな純粋関数として .rodata に
 *   相当する定数計算式で表現する（分岐だけの単純な式なので実体は生成された
 *   コードそのものであり、cursor フォントのグリフのような外部リソースは
 *   一切参照しない）。起動時に 1 度だけこれを実行して 0/1 の画素配列
 *   （最大 32×32 = 1KB）を組み立て、X のワイヤフォーマット
 *   （bitmap-format-scanline-unit/pad と bit-order に従ったパック）に変換して
 *   xcb_put_image へ渡す。生成は起動時 1 回だけで、以後は cursor_get() が
 *   キャッシュした xcb_cursor_t を返すだけ (§4.7)。
 */
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

/* ================================================================== *
 * カーソルの寸法 (SPEC §4.7: 32x32 以内。16x16 で十分)
 * ================================================================== */

#define CW 16u
#define CH 16u

enum pixel { PX_NONE = 0, PX_WHITE = 1, PX_BLACK = 2 };

static xcb_cursor_t g_cursors[CURSOR_COUNT];
static bool         g_have[CURSOR_COUNT];

/* ================================================================== *
 * 形状定義 (純粋関数。x,y は 0..CW-1 / 0..CH-1)
 * ================================================================== */

/*
 * 矢印 (SPEC §4.7: 黒の塗り + 白の縁取り、先端を原点に置く)。
 * 直角三角形 (0,0)-(0,13)-(13,13)。左辺・斜辺・底辺が黒、内側が白。
 * hotspot は要求どおり先端 (0,0)。
 */
static enum pixel shape_arrow(int x, int y)
{
	if (x < 0 || y < 0 || x > 13 || y > 13 || x > y)
		return PX_NONE;
	if (x == 0 || x == y || y == 13)
		return PX_BLACK;
	return PX_WHITE;
}

/* 水平方向の両矢印 (← →)。SIZE_W / SIZE_E で使う。中心 (8,8)。 */
static enum pixel shape_horiz(int x, int y)
{
	if (x >= 0 && x <= 5) {
		int top = 7 - x, bot = 8 + x;
		if (y < top || y > bot)
			return PX_NONE;
		return (y == top || y == bot) ? PX_BLACK : PX_WHITE;
	}
	if (x >= 10 && x <= 15) {
		int d = 15 - x, top = 7 - d, bot = 8 + d;
		if (y < top || y > bot)
			return PX_NONE;
		return (y == top || y == bot) ? PX_BLACK : PX_WHITE;
	}
	if (x >= 6 && x <= 9) {
		if (y == 6 || y == 9)
			return PX_BLACK;
		if (y == 7 || y == 8)
			return PX_WHITE;
	}
	return PX_NONE;
}

/* 垂直方向の両矢印 (↑ ↓)。SIZE_N / SIZE_S で使う。horiz の転置。 */
static enum pixel shape_vert(int x, int y)
{
	return shape_horiz(y, x);
}

/* 移動カーソル：水平・垂直を重ね合わせた十字矢印 (SIZE_MOVE)。 */
static enum pixel shape_move(int x, int y)
{
	enum pixel h = shape_horiz(x, y);
	enum pixel v = shape_vert(x, y);
	if (h == PX_BLACK || v == PX_BLACK)
		return PX_BLACK;
	if (h == PX_WHITE || v == PX_WHITE)
		return PX_WHITE;
	return PX_NONE;
}

/* 斜め方向 \ (NW-SE)。太さ 2px の対角線 + 両端の小さな返し。 */
static enum pixel shape_diag_nwse(int x, int y)
{
	if (x >= 0 && x <= 15) {
		if (y == x || y == x + 1)
			return PX_BLACK;
	}
	if ((x == 3 && y == 1) || (x == 1 && y == 3))
		return PX_BLACK;
	if ((x == 12 && y == 14) || (x == 14 && y == 12))
		return PX_BLACK;
	return PX_NONE;
}

/* 斜め方向 / (NE-SW)。太さ 2px の対角線 + 両端の小さな返し。 */
static enum pixel shape_diag_nesw(int x, int y)
{
	if (x >= 0 && x <= 15) {
		if (y == 15 - x || y == 14 - x)
			return PX_BLACK;
	}
	if ((x == 13 && y == 3) || (x == 11 && y == 1))
		return PX_BLACK;
	if ((x == 3 && y == 13) || (x == 1 && y == 11))
		return PX_BLACK;
	return PX_NONE;
}

/*
 * 砂時計 (SIZE_WAIT)。上下の横棒 + 中心へすぼまる 2 本の斜辺、内側は白。
 * margin(y) は中心行 (7/8) に近づくほど大きくなり、外周と内側の縁を作る。
 */
static enum pixel shape_wait(int x, int y)
{
	int margin;

	if (y < 2 || y > 13)
		return PX_NONE;
	if (y == 2 || y == 13)
		return (x >= 3 && x <= 12) ? PX_BLACK : PX_NONE;
	if (y == 7 || y == 8)
		return (x == 7 || x == 8) ? PX_BLACK : PX_NONE;

	margin = (y <= 6) ? (y - 2) : (13 - y);   /* 1..4、中心ほど大きい */
	if (x < 3 + margin || x > 12 - margin)
		return PX_NONE;
	return (x == 3 + margin || x == 12 - margin) ? PX_BLACK : PX_WHITE;
}

typedef enum pixel (*shape_fn)(int x, int y);

struct cursor_def {
	shape_fn fn;
	int16_t  hot_x, hot_y;
};

/* CURSOR_* (draw.h) の順に対応させる */
static const struct cursor_def g_defs[CURSOR_COUNT] = {
	[CURSOR_ARROW]   = { shape_arrow,      0,  0 },
	[CURSOR_SIZE_NW] = { shape_diag_nwse,  8,  8 },
	[CURSOR_SIZE_N]  = { shape_vert,       8,  8 },
	[CURSOR_SIZE_NE] = { shape_diag_nesw,  8,  8 },
	[CURSOR_SIZE_W]  = { shape_horiz,      8,  8 },
	[CURSOR_SIZE_E]  = { shape_horiz,      8,  8 },
	[CURSOR_SIZE_SW] = { shape_diag_nesw,  8,  8 },
	[CURSOR_SIZE_S]  = { shape_vert,       8,  8 },
	[CURSOR_SIZE_SE] = { shape_diag_nwse,  8,  8 },
	[CURSOR_MOVE]    = { shape_move,       8,  8 },
	[CURSOR_WAIT]    = { shape_wait,       8,  8 },
};

/* ================================================================== *
 * ワイヤフォーマットへのパック
 *
 * X のコアプロトコルは 1bit 画像を「bitmap-format-scanline-unit/pad」と
 * 「bitmap-format-bit-order」「image-byte-order」に従ってパックすることを
 * 要求する (X11 プロトコル仕様 §Image Format)。サーバごとに値が異なり
 * 得るため、xcb_get_setup() の実測値から都度組み立てる。
 * ================================================================== */

static uint8_t *pack_xy_bitmap(const uint8_t *px, uint16_t w, uint16_t h,
                               uint32_t *out_len)
{
	const xcb_setup_t *setup = xcb_get_setup(wm.conn);
	uint32_t unit  = setup->bitmap_format_scanline_unit;
	uint32_t pad   = setup->bitmap_format_scanline_pad;
	bool bit_msb   = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
	bool byte_msb  = setup->image_byte_order == XCB_IMAGE_ORDER_MSB_FIRST;
	uint32_t bytes_per_unit = unit / 8u;
	uint32_t stride_bits, stride_bytes, len;
	uint8_t *buf;
	uint16_t x, y;

	if (unit == 0)
		unit = 8;
	if (pad == 0)
		pad = 8;
	bytes_per_unit = unit / 8u;

	stride_bits  = ((uint32_t)w + pad - 1u) / pad * pad;
	stride_bytes = stride_bits / 8u;
	len = stride_bytes * h;

	buf = calloc(1, len ? len : 1u);
	if (!buf) {
		*out_len = 0;
		return NULL;
	}

	for (y = 0; y < h; y++) {
		uint8_t *line = buf + (uint32_t)y * stride_bytes;
		for (x = 0; x < w; x++) {
			uint32_t group, bit_in_grp, bit_num, byte_in_u, bit_in_byte;
			uint32_t actual_byte, byte_off;

			if (!px[(uint32_t)y * w + x])
				continue;

			group       = x / unit;
			bit_in_grp  = x % unit;
			bit_num     = bit_msb ? (unit - 1u - bit_in_grp) : bit_in_grp;
			byte_in_u   = bit_num / 8u;
			bit_in_byte = bit_num % 8u;
			actual_byte = byte_msb ? (bytes_per_unit - 1u - byte_in_u) : byte_in_u;
			byte_off    = group * bytes_per_unit + actual_byte;

			if (byte_off < stride_bytes)
				line[byte_off] = (uint8_t)(line[byte_off] | (1u << bit_in_byte));
		}
	}

	*out_len = len;
	return buf;
}

/* px (0/1 の平面) を新規 depth=1 Pixmap へ転送する */
static xcb_pixmap_t upload_plane(const uint8_t *px, uint16_t w, uint16_t h)
{
	xcb_pixmap_t pm;
	xcb_gcontext_t gc;
	uint32_t gc_val = 0;
	uint8_t *wire;
	uint32_t wire_len;

	pm = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, 1, pm, wm.root, w, h);

	gc = xcb_generate_id(wm.conn);
	xcb_create_gc(wm.conn, gc, pm, XCB_GC_FUNCTION, &gc_val);

	wire = pack_xy_bitmap(px, w, h, &wire_len);
	if (wire) {
		xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_XY_PIXMAP, pm, gc,
		             w, h, 0, 0, 0, 1, wire_len, wire);
		free(wire);
	}

	xcb_free_gc(wm.conn, gc);
	return pm;
}

/* ================================================================== *
 * 公開 API
 * ================================================================== */

static xcb_cursor_t build_one(const struct cursor_def *def)
{
	uint8_t src[CW * CH];
	uint8_t mask[CW * CH];
	xcb_pixmap_t src_pm, mask_pm;
	xcb_cursor_t cid;
	uint16_t x, y;

	for (y = 0; y < CH; y++) {
		for (x = 0; x < CW; x++) {
			enum pixel p = def->fn((int)x, (int)y);
			src[y * CW + x]  = (p == PX_BLACK) ? 1u : 0u;
			mask[y * CW + x] = (p != PX_NONE)  ? 1u : 0u;
		}
	}

	src_pm  = upload_plane(src, CW, CH);
	mask_pm = upload_plane(mask, CW, CH);

	cid = xcb_generate_id(wm.conn);
	/* 配色は §4.1 の影響を受けない固定値 (§4.7): 前景=黒, 背景=白 */
	xcb_create_cursor(wm.conn, cid, src_pm, mask_pm,
	                  0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF,
	                  (uint16_t)def->hot_x, (uint16_t)def->hot_y);

	xcb_free_pixmap(wm.conn, src_pm);
	xcb_free_pixmap(wm.conn, mask_pm);

	return cid;
}

void cursor_init(void)
{
	int i;
	uint32_t val;

	memset(g_cursors, 0, sizeof(g_cursors));
	memset(g_have, 0, sizeof(g_have));

	for (i = 0; i < CURSOR_COUNT; i++) {
		if (!g_defs[i].fn)
			continue;
		g_cursors[i] = build_one(&g_defs[i]);
		g_have[i] = true;
	}

	/* デスクトップ（ルート）が既定の X の "X" ポインタを出さないようにする */
	val = g_have[CURSOR_ARROW] ? g_cursors[CURSOR_ARROW] : XCB_NONE;
	xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_CURSOR, &val);

	xcb_flush(wm.conn);
}

void cursor_fini(void)
{
	int i;

	for (i = 0; i < CURSOR_COUNT; i++) {
		if (g_have[i]) {
			xcb_free_cursor(wm.conn, g_cursors[i]);
			g_have[i] = false;
		}
	}
}

xcb_cursor_t cursor_get(int which)
{
	if (which < 0 || which >= CURSOR_COUNT || !g_have[which])
		return g_have[CURSOR_ARROW] ? g_cursors[CURSOR_ARROW] : XCB_NONE;
	return g_cursors[which];
}
