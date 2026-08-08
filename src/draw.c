/*
 * draw.c - 描画プリミティブ (SPEC §4.3, §4.4)
 *
 * cairo/XRender は使わない。libxcb のコアプロトコルのみで描く。
 *
 * 色の表現について: wm.cfg.color[] および draw_* に渡す uint32_t は
 * 0x00RRGGBB。本 WM は Colormap 変換を一切持たない設計であり（config.c /
 * theme.c にも存在しない）、事実上すべての現代的 X サーバが 24/32bit
 * TrueColor・標準 RGB マスク (0xFF0000/0x00FF00/0x0000FF) であることを
 * 前提に、この値をそのまま GC の foreground pixel として使う。
 * PseudoColor 等の非対応は既知の制約として報告する。
 *
 * GC は「用途ごとに使い分ける」（draw.h）が、色は呼び出しごとに変わるため
 * GC 自体は 1 個だけ作り、描画のたびに XCB_GC_FOREGROUND を ChangeGC で
 * 差し替えて使い回す。GC を呼び出しごとに Create/Free することはしない。
 */
#include "w98wm.h"

/* 描画用の唯一の GC。draw_init で 1 度だけ作る。 */
static xcb_gcontext_t g_gc;
static bool g_ready = false;

void draw_init(void)
{
	if (g_ready)
		return;

	g_gc = xcb_generate_id(wm.conn);
	uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND |
	               XCB_GC_GRAPHICS_EXPOSURES;
	uint32_t values[3];
	values[0] = wm.screen->black_pixel;
	values[1] = wm.screen->white_pixel;
	values[2] = 0; /* GraphicsExposures 不要 (§: 余計な Expose を避ける) */
	xcb_create_gc(wm.conn, g_gc, wm.root, mask, values);

	g_ready = true;
}

void draw_fini(void)
{
	if (!g_ready)
		return;
	xcb_free_gc(wm.conn, g_gc);
	g_ready = false;
}

/* GC の前景色を差し替える。draw_* の各関数から呼ぶ内部ヘルパ。 */
static void set_fg(uint32_t color)
{
	uint32_t value = color;
	xcb_change_gc(wm.conn, g_gc, XCB_GC_FOREGROUND, &value);
}

void draw_rect(xcb_drawable_t d, int16_t x, int16_t y,
              uint16_t w, uint16_t h, uint32_t color)
{
	if (w == 0 || h == 0)
		return;
	set_fg(color);
	xcb_rectangle_t r = { .x = x, .y = y, .width = w, .height = h };
	/* PolyFillRectangle は width×height のピクセルをそのまま塗る
	 * （PolyRectangle と違って境界線の重複による +1 のズレは無い）。 */
	xcb_poly_fill_rectangle(wm.conn, d, g_gc, 1, &r);
}

void draw_line(xcb_drawable_t d, int16_t x1, int16_t y1,
              int16_t x2, int16_t y2, uint32_t color)
{
	set_fg(color);
	xcb_point_t pts[2] = {
		{ .x = x1, .y = y1 },
		{ .x = x2, .y = y2 },
	};
	xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, 2, pts);
}

void draw_frame_rect(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h, uint32_t color)
{
	if (w == 0 || h == 0)
		return;
	set_fg(color);
	/*
	 * PolyRectangle は [x,y]-[x+width,y]-[x+width,y+height]-[x,y+height]
	 * を結ぶ線分として描く（X11 コアプロトコルの定義）。つまり実際に
	 * 占有するピクセルは width+1 × height+1 になる。呼び出し側が期待する
	 * 「w×h ピクセルを外周にちょうど 1px で囲う」ためには width/height に
	 * -1 する必要がある（PolyFillRectangle の +0 とは異なる、よくある
	 * X11 の落とし穴）。
	 */
	xcb_rectangle_t r = {
		.x = x, .y = y,
		.width  = (uint16_t)(w - 1),
		.height = (uint16_t)(h - 1),
	};
	xcb_poly_rectangle(wm.conn, d, g_gc, 1, &r);
}

/* ================================================================== *
 * ベベル (SPEC §4.3)
 *
 * DrawEdge の再現。1 つの「輪」は L 字の 2 本の PolyLine で描く:
 *   TL側: 左辺全長 + 上辺（右端 1px を除く）
 *   BR側: 下辺全長 + 右辺全長
 * この配分（TL 側が右端/下端の 1px を持たない）は Wine の user32
 * DrawEdge 実装に倣った。実機のコーナーピクセルの正確な帰属は
 * §4.0 の基準スクリーンショットで最終確認する（未検証）。
 * ================================================================== */

/* x,y,w,h の外周に 1 輪だけ描く。w<2 または h<2 の場合は縮退させる。 */
static void draw_edge_ring(xcb_drawable_t d, int16_t x, int16_t y,
                           uint16_t w, uint16_t h,
                           uint32_t tl_color, uint32_t br_color)
{
	if (w == 0 || h == 0)
		return;

	int16_t right  = (int16_t)(x + w - 1);
	int16_t bottom = (int16_t)(y + h - 1);

	/* TL: 左辺 (y+h-2 .. y) + 上辺 (x .. x+w-2)。1px 幅/高の場合は
	 * 該当辺を描かない（1 点に縮退する線は不要）。 */
	set_fg(tl_color);
	if (w >= 2 && h >= 2) {
		xcb_point_t tl[3] = {
			{ .x = x, .y = (int16_t)(bottom - 1) },
			{ .x = x, .y = y },
			{ .x = (int16_t)(right - 1), .y = y },
		};
		xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, 3, tl);
	} else if (h >= 2) {
		/* w==1: 上辺が無い。左辺のみ。 */
		xcb_point_t tl[2] = {
			{ .x = x, .y = (int16_t)(bottom - 1) },
			{ .x = x, .y = y },
		};
		xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, 2, tl);
	} else if (w >= 2) {
		/* h==1: 左辺が無い。上辺のみ。 */
		xcb_point_t tl[2] = {
			{ .x = x, .y = y },
			{ .x = (int16_t)(right - 1), .y = y },
		};
		xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, 2, tl);
	}

	/* BR: 下辺全長 + 右辺全長 */
	set_fg(br_color);
	xcb_point_t br[3] = {
		{ .x = x,     .y = bottom },
		{ .x = right, .y = bottom },
		{ .x = right, .y = y },
	};
	xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, 3, br);
}

void draw_bevel(xcb_drawable_t d, int16_t x, int16_t y,
               uint16_t w, uint16_t h, enum bevel_kind kind)
{
	if (w == 0 || h == 0)
		return;

	const uint32_t *col = wm.cfg.color;

	switch (kind) {
	case BEVEL_RAISED:
		/* 外: light/dkshadow, 内: hilight/shadow (§4.3 の表・図と一致) */
		draw_edge_ring(d, x, y, w, h,
		              col[THEME_LIGHT], col[THEME_DKSHADOW]);
		if (w >= 2 && h >= 2)
			draw_edge_ring(d, (int16_t)(x + 1), (int16_t)(y + 1),
			              (uint16_t)(w - 2), (uint16_t)(h - 2),
			              col[THEME_HILIGHT], col[THEME_SHADOW]);
		break;
	case BEVEL_SUNKEN:
		/* 外: shadow/hilight, 内: dkshadow/light */
		draw_edge_ring(d, x, y, w, h,
		              col[THEME_SHADOW], col[THEME_HILIGHT]);
		if (w >= 2 && h >= 2)
			draw_edge_ring(d, (int16_t)(x + 1), (int16_t)(y + 1),
			              (uint16_t)(w - 2), (uint16_t)(h - 2),
			              col[THEME_DKSHADOW], col[THEME_LIGHT]);
		break;
	case BEVEL_PRESSED:
		/* 外: shadow/face, 内: dkshadow/face。押下ボタンは明部を持たない。
		 * 内容を 1px 右下へずらすのは呼び出し側 (deco.c) の責務。 */
		draw_edge_ring(d, x, y, w, h,
		              col[THEME_SHADOW], col[THEME_FACE]);
		if (w >= 2 && h >= 2)
			draw_edge_ring(d, (int16_t)(x + 1), (int16_t)(y + 1),
			              (uint16_t)(w - 2), (uint16_t)(h - 2),
			              col[THEME_DKSHADOW], col[THEME_FACE]);
		break;
	case BEVEL_BUMP:
		/* 1px のみ。hilight/shadow。内側の輪は無い。 */
		draw_edge_ring(d, x, y, w, h,
		              col[THEME_HILIGHT], col[THEME_SHADOW]);
		break;
	}
}

/* ================================================================== *
 * 水平グラデーション (SPEC §4.4)
 * ================================================================== */

/*
 * 補間規則はここ 1 関数に閉じ込める（§4.4 の注記どおり、この規則自体が
 * 実機未検証であり比較の結果変わり得るのはここだけ、という保証のため）。
 *
 * 列 i (0-origin, 0<=i<w) のチャネル値 = lo + (hi-lo)*i/(w-1)。
 * 丸めは「四捨五入（0.5 は絶対値が大きくなる方向へ、すなわち符号に応じて
 * 上または下へ）」を採用する。w<=1 のときは i/(w-1) が未定義になるため、
 * 特別扱いして常に lo を返す（1 列しか無いので左端の色を採用する）。
 */
static uint8_t lerp_channel(uint8_t lo, uint8_t hi, uint16_t i, uint16_t w)
{
	if (w <= 1)
		return lo;

	int32_t diff = (int32_t)hi - (int32_t)lo;
	int32_t num  = diff * (int32_t)i;
	int32_t den  = (int32_t)w - 1;
	int32_t half = den / 2;

	int32_t q;
	if (num >= 0)
		q = (num + half) / den;
	else
		q = -((-num + half) / den);

	return (uint8_t)(lo + q);
}

/* ΔR+ΔG+ΔB+1 の上限。任意の色でも 255*3+1=766 を超えない (§4.4)。 */
#define GRADIENT_MAX_RUNS 768

void draw_gradient_h(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h, uint32_t left, uint32_t right)
{
	if (w == 0 || h == 0)
		return;

	uint8_t lr = (uint8_t)(left  >> 16), lg = (uint8_t)(left  >> 8), lb = (uint8_t)left;
	uint8_t rr = (uint8_t)(right >> 16), rg = (uint8_t)(right >> 8), rb = (uint8_t)right;

	/* [begin_x, run色] の列を作る。境界は R/G/B いずれかが変化する位置の
	 * 和集合。X11 コアプロトコルでは 1 回の PolyFillRectangle は単一の GC
	 * (単一色) にしか適用できないため、各 run ごとに ChangeGC +
	 * PolyFillRectangle を 1 回ずつ発行する。リクエスト総数は幅に依存せず
	 * ΔR+ΔG+ΔB+1 (<=766) で頭打ちになる。 */
	uint16_t run_start = 0;
	uint8_t prev_r = lr, prev_g = lg, prev_b = lb;
	uint16_t n_runs = 0;

	for (uint16_t i = 1; i <= w; i++) {
		uint8_t cr, cg, cb;
		bool boundary;
		if (i == w) {
			boundary = true;
			cr = cg = cb = 0; /* 使わない */
		} else {
			cr = lerp_channel(lr, rr, i, w);
			cg = lerp_channel(lg, rg, i, w);
			cb = lerp_channel(lb, rb, i, w);
			boundary = (cr != prev_r || cg != prev_g || cb != prev_b);
		}

		if (boundary) {
			uint32_t pixel = ((uint32_t)prev_r << 16) |
			                 ((uint32_t)prev_g << 8) |
			                 (uint32_t)prev_b;
			set_fg(pixel);
			xcb_rectangle_t r = {
				.x = (int16_t)(x + run_start),
				.y = y,
				.width  = (uint16_t)(i - run_start),
				.height = h,
			};
			xcb_poly_fill_rectangle(wm.conn, d, g_gc, 1, &r);

			n_runs++;
			/* 8bit チャネル 3 本の差分では理論上 766 を超えない
			 * (ΔR+ΔG+ΔB+1 <= 766 < GRADIENT_MAX_RUNS)。この分岐は
			 * 数学的には到達しないはずだが、想定外の入力に対して
			 * バッファ/リクエスト数を無限に増やさないための安全弁。
			 * 到達した場合は残り幅を「次の run の色」(cr,cg,cb) で
			 * 一括して塗り切って抜ける。以降さらに変化点があっても
			 * それは無視されるため補間とは完全一致しなくなるが、
			 * ギャップや配列オーバーランは起きない。 */
			if (n_runs >= GRADIENT_MAX_RUNS && i < w) {
				uint32_t tail_pixel = ((uint32_t)cr << 16) |
				                      ((uint32_t)cg << 8) |
				                      (uint32_t)cb;
				set_fg(tail_pixel);
				xcb_rectangle_t tail = {
					.x = (int16_t)(x + i),
					.y = y,
					.width  = (uint16_t)(w - i),
					.height = h,
				};
				xcb_poly_fill_rectangle(wm.conn, d, g_gc, 1, &tail);
				return;
			}

			run_start = i;
			prev_r = cr; prev_g = cg; prev_b = cb;
		}
	}
}

/* ================================================================== *
 * 1bit グリフの描画 (キャプションボタンのアイコン等)
 *
 * bits のビット配置（draw.h に規定が無いため、ここで定義する契約）:
 *   - 行優先、各行は 1 バイト境界にパディング（stride = (w+7)/8 バイト）
 *   - MSB ファースト: バイトの bit7 が各行の先頭（最も左の）列
 *   - ビットが立っている画素だけを前景色で描く。0 の画素には触れない
 *     （透過。キャプションのグラデーション上に重ねて描くための仕様）。
 * font.c / deco.c 側がこの並びで bits を用意する必要がある。ここは
 * draw.h に明文化が無い箇所なので、報告に記載する。
 * ================================================================== */

/* 1 回の xcb_poly_point でまとめて送る最大点数。stack 上に置くので
 * 適度な大きさに抑え、それを超える分は複数回に分けて flush する
 * （drop はしない）。 */
#define GLYPH_POINT_BATCH 256

void draw_glyph_bits(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h,
                     const uint8_t *bits, uint32_t color)
{
	if (w == 0 || h == 0 || !bits)
		return;

	set_fg(color);

	size_t stride = ((size_t)w + 7) / 8;
	xcb_point_t pts[GLYPH_POINT_BATCH];
	unsigned n = 0;

	for (uint16_t row = 0; row < h; row++) {
		const uint8_t *rowp = bits + (size_t)row * stride;
		for (uint16_t col = 0; col < w; col++) {
			uint8_t byte = rowp[col / 8];
			uint8_t bit  = (uint8_t)(0x80u >> (col % 8));
			if (!(byte & bit))
				continue;

			pts[n].x = (int16_t)(x + col);
			pts[n].y = (int16_t)(y + row);
			n++;
			if (n == GLYPH_POINT_BATCH) {
				xcb_poly_point(wm.conn, XCB_COORD_MODE_ORIGIN,
				               d, g_gc, n, pts);
				n = 0;
			}
		}
	}
	if (n > 0)
		xcb_poly_point(wm.conn, XCB_COORD_MODE_ORIGIN, d, g_gc, n, pts);
}
