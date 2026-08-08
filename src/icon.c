/*
 * icon.c - _NET_WM_ICON と WM_HINTS のアイコン取得 (SPEC §4.4.1)
 *
 * 対応する仕様: docs/SPEC.md §4.4.1（クライアント側 RAM を削る代わりに
 * X サーバ側リソースを使う設計）、§4.4（deco.c での描き方）、§9.1（メモリ目標）
 *
 * ──────────────────────────────────────────────────────────────────
 * 【最初に読むこと】このファイルの慎重さは「やりすぎ」に見えるが全部理由がある
 * ──────────────────────────────────────────────────────────────────
 * _NET_WM_ICON は 256x256 の 1 枚だけで 256KB、複数枚積んだプロパティ全体では
 * 数 MB になり得る。素朴に GetProperty で丸ごと読むと、ウィンドウ 1 枚の
 * アイコン取得だけで本プロジェクトの 1MB メモリ目標 (§9.1) を一時的にでも
 * 破ってしまう。だから：
 *   1. プロパティは long_offset/long_length で分割して読む。
 *      まずヘッダ 2 語（幅・高さ）だけを読んでアイコン一覧を走査し、
 *      採用する 1 枚の画素データだけを、しかも 16x16 の出力 1 行ぶんずつ
 *      （最大 256 語 = 1KB）読む。プロパティ全体を一度にメモリへ載せない。
 *   2. 分割して読んでいる最中にプロパティが差し替わるレースを検出する。
 *      進捗表示のように頻繁にアイコンを差し替えるアプリがまさにこれをやる。
 *      各リプライの「オフセット + 取得語数 + bytes_after」を初回確定値と
 *      突き合わせ、不一致なら最初から読み直す（最大 2 回まで）。
 *   3. 保持は 1 クライアントあたり Pixmap 1 枚 + マスク 1 枚のみ。
 *      差し替え時は新しい方を先に作ってから古い方を FreePixmap する。
 *
 * WM_HINTS.icon_pixmap / icon_mask はクライアント所有の Drawable ID である。
 * icccm.c は意図的にこの 2 フィールドを無視する（icccm.c 内のコメント参照）。
 * ここで扱う。ICCCM はこの Pixmap のサイズも深度も規定しないため
 * （慣習的に深度 1 が多い）、画面深度と異なる Drawable へ CopyArea すると
 * BadMatch になる。深度 1 は CopyPlane、画面深度なら CopyArea、
 * それ以外の深度は無視する。
 *
 * struct client のアイコン系フィールドの所有権 (w98wm.h 参照):
 *   icon_pix / icon_mask         : WM が CreatePixmap した 16x16。
 *                                  差し替え・破棄時に必ず FreePixmap する。
 *   wmh_icon_pix / wmh_icon_mask : WM_HINTS 由来のクライアント所有 ID。
 *                                  絶対に FreePixmap しない（他プロセスの
 *                                  Drawable を破壊する）。深度・サイズが
 *                                  そのまま使える場合だけ、コピーを作らずに
 *                                  ここへ直接 ID を入れる（1 クライアント
 *                                  1 Pixmap の予算を無駄なく守るため）。
 *                                  変換が要る場合は代わりに icon_pix/_mask
 *                                  を作り、こちらは XCB_NONE のままにする
 *                                  （deco.c の draw_icon は icon_pix が
 *                                  XCB_NONE の時だけ wmh_icon_pix を見る）。
 *
 * アルファの扱い: 本 WM はコンポジタを持たない。ARGB を実際のキャプション
 * 背景に対して合成できれば一番きれいだが、キャプション背景は水平グラデーション
 * （§4.4）なので「背景色」という単一の値が存在せず、事前合成のしようがない。
 * そのため半透明は 1 ビットへ量子化する：alpha>=128 を不透明、それ未満を
 * 透明として扱い、deco.c の CopyArea 用クリップマスクに使う。
 * 半透明の縁（アンチエイリアス）はギザギザになるが、これは実装の手抜きではなく
 * 「合成できないので閾値化する」という設計上の割り切りである。
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

#include "w98wm.h"

#define ICON_SZ              16u   /* deco.c が描く固定サイズ (§4.2, §4.4) */
#define ICON_ENTRY_MAX_SIDE 256u   /* 一辺 256 超のエントリは走査対象外 (§4.4.1) */
#define ICON_PROP_MAX_BYTES (4u * 1024u * 1024u) /* プロパティ全体の防御的上限 */
#define ICON_RETRY_MAX          2  /* レース検出時の再読み込み上限 */

/*
 * WM_HINTS.flags のビット (ICCCM §4.1.2.4)。icccm.c にも同じ値の private な
 * enum があるが file-scope static でヘッダ経由で共有できないため、
 * プロトコル上固定の値をここでも複製する。
 */
#define WMH_ICON_PIXMAP (1u << 2)
#define WMH_ICON_MASK   (1u << 5)
#define WMH_LEN         9u   /* CARD32 9 語 */

/* ================================================================== *
 * 純粋なヘルパ（xcb 通信を伴わない）
 * ================================================================== */

/*
 * 出力側 16x16 の座標 dst_i を、幅/高さ src_n の入力側座標へ最近傍で
 * 対応させる。縮小・拡大のどちらでも同じ式で正しく動く。
 */
static uint32_t nearest_index(uint32_t dst_i, uint32_t dst_n, uint32_t src_n)
{
	uint32_t idx = (dst_i * 2u * src_n + src_n) / (2u * dst_n);

	if (idx >= src_n)
		idx = src_n - 1u;
	return idx;
}

/*
 * 深度 1 の XYPixmap/XYBitmap ワイヤ表現における、画素 x が
 * 位置するバイトオフセットとバイト内ビット位置を求める。
 * bitmap-format-scanline-unit/pad と bitmap-format-bit-order・
 * image-byte-order の組み合わせはサーバごとに違う (X11 プロトコル仕様の
 * Image Format 節)。cursor.c の pack_xy_bitmap() と同じ考え方。
 */
static void xy_bit_pos(uint32_t unit, bool bit_msb, bool byte_msb,
                       uint16_t x, uint32_t *byte_off, uint8_t *bit_in_byte)
{
	uint32_t bytes_per_unit = unit / 8u;
	uint32_t group = (uint32_t)x / unit;
	uint32_t bit_in_grp = (uint32_t)x % unit;
	uint32_t bit_num = bit_msb ? (unit - 1u - bit_in_grp) : bit_in_grp;
	uint32_t byte_in_u = bit_num / 8u;
	uint32_t bib = bit_num % 8u;
	uint32_t actual_byte = byte_msb ? (bytes_per_unit - 1u - byte_in_u) : byte_in_u;

	*byte_off = group * bytes_per_unit + actual_byte;
	*bit_in_byte = (uint8_t)bib;
}

/* px (0/1 の w*h 平面) を depth=1 PutImage 用のワイヤ表現へパックする。
 * 呼び出し元が free() すること。失敗時は NULL。 */
static uint8_t *xy_pack(const uint8_t *px, uint16_t w, uint16_t h, uint32_t *out_len)
{
	const xcb_setup_t *setup = xcb_get_setup(wm.conn);
	uint32_t unit = setup->bitmap_format_scanline_unit;
	uint32_t pad = setup->bitmap_format_scanline_pad;
	bool bit_msb = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
	bool byte_msb = setup->image_byte_order == XCB_IMAGE_ORDER_MSB_FIRST;
	uint32_t stride_bits, stride_bytes, len;
	uint8_t *buf;
	uint16_t x, y;

	if (unit == 0)
		unit = 8;
	if (pad == 0)
		pad = 8;

	stride_bits = ((uint32_t)w + pad - 1u) / pad * pad;
	stride_bytes = stride_bits / 8u;
	len = stride_bytes * h;

	buf = calloc(1, len ? len : 1u);
	if (buf == NULL) {
		*out_len = 0;
		return NULL;
	}

	for (y = 0; y < h; y++) {
		uint8_t *line = buf + (uint32_t)y * stride_bytes;

		for (x = 0; x < w; x++) {
			uint32_t byte_off;
			uint8_t bit_in_byte;

			if (!px[(uint32_t)y * w + x])
				continue;
			xy_bit_pos(unit, bit_msb, byte_msb, x, &byte_off, &bit_in_byte);
			if (byte_off < stride_bytes)
				line[byte_off] = (uint8_t)(line[byte_off] | (1u << bit_in_byte));
		}
	}

	*out_len = len;
	return buf;
}

/* 指定した画面深度の bits-per-pixel を setup の pixmap_formats から引く。
 * 見つからなければ 0。 */
static uint32_t icon_depth_bpp(uint8_t depth)
{
	const xcb_setup_t *setup = xcb_get_setup(wm.conn);
	xcb_format_iterator_t it = xcb_setup_pixmap_formats_iterator(setup);

	for (; it.rem > 0; xcb_format_next(&it)) {
		if (it.data->depth == depth)
			return it.data->bits_per_pixel;
	}
	return 0;
}

/* ================================================================== *
 * _NET_WM_ICON: 分割読み込みとレース検出 (SPEC §4.4.1)
 * ================================================================== */

struct icon_prop_ctx {
	xcb_window_t win;
	uint32_t total_words;   /* 0 = まだ未確定 */
};

/*
 * offset/length は 32bit ワード単位。各リプライで
 * 「offset + 取得語数 + bytes_after(バイト)/4」を初回確定値と突き合わせ、
 * 食い違えば *race に true を立てて NULL を返す（呼び出し元は最初から
 * 読み直す）。型や存在確認に失敗した場合は race を立てずに NULL を返す
 * （その場合は「_NET_WM_ICON を使わない」という判断でよい）。
 * 返したリプライは呼び出し元が free() すること。
 */
static xcb_get_property_reply_t *icon_read_words(struct icon_prop_ctx *ctx,
                                                  uint32_t offset, uint32_t length,
                                                  bool *race)
{
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *r;
	uint32_t got_words, total_now;

	*race = false;
	ck = xcb_get_property(wm.conn, 0, ctx->win, atoms[ATOM_NET_WM_ICON],
	                      XCB_ATOM_CARDINAL, offset, length);
	r = xcb_get_property_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return NULL;
	if (r->type != XCB_ATOM_CARDINAL || r->format != 32) {
		free(r);
		return NULL;
	}

	got_words = (uint32_t)xcb_get_property_value_length(r) / 4u;
	total_now = offset + got_words + r->bytes_after / 4u;
	if (ctx->total_words == 0) {
		ctx->total_words = total_now;
	} else if (total_now != ctx->total_words) {
		free(r);
		*race = true;
		return NULL;
	}
	return r;
}

enum icon_scan_status { ICON_SCAN_OK, ICON_SCAN_RACE, ICON_SCAN_NONE };

/*
 * ヘッダ（幅・高さの 2 語ずつ）だけを走査してエントリ一覧を確認し、
 * 採用規則 (§4.4.1) に従って 1 枚選ぶ。選んだエントリの画素データの
 * ワードオフセットと幅・高さを返す。
 */
static enum icon_scan_status icon_scan(struct icon_prop_ctx *ctx,
                                       uint32_t *sel_off, uint32_t *sel_w, uint32_t *sel_h)
{
	xcb_get_property_reply_t *r;
	bool race = false;
	uint32_t pos = 0;
	bool have_exact = false, have_down = false, have_up = false;
	uint32_t exact_off = 0, exact_w = 0, exact_h = 0;
	uint32_t down_off = 0, down_w = 0, down_h = 0, down_area = 0;
	uint32_t up_off = 0, up_w = 0, up_h = 0, up_area = 0;

	r = icon_read_words(ctx, 0, 2, &race);
	if (race)
		return ICON_SCAN_RACE;
	if (r == NULL)
		return ICON_SCAN_NONE;   /* プロパティ無し、または型違い */

	/* 総バイト数が確定した時点で 4MB の防御的上限を確認 (§4.4.1) */
	if ((uint64_t)ctx->total_words * 4u > ICON_PROP_MAX_BYTES) {
		free(r);
		return ICON_SCAN_NONE;
	}

	for (;;) {
		uint32_t w, h, entry_words;
		const uint32_t *hdr;
		uint64_t words64;

		if ((uint32_t)xcb_get_property_value_length(r) < 8u) {
			free(r);
			r = NULL;
			break;   /* ヘッダ 2 語に満たない。走査を打ち切る */
		}
		hdr = xcb_get_property_value(r);
		w = hdr[0];
		h = hdr[1];
		free(r);
		r = NULL;

		if (w == 0 || h == 0)
			break;   /* 壊れたエントリ。以降は解釈できない */

		/* w*h は 32bit を溢れ得るので 64bit で計算してから境界を確認する */
		words64 = (uint64_t)w * (uint64_t)h;
		if (pos + 2 > ctx->total_words || words64 > ctx->total_words - pos - 2)
			break;   /* エントリがプロパティ範囲をはみ出す＝壊れている */
		entry_words = (uint32_t)words64;

		if (w <= ICON_ENTRY_MAX_SIDE && h <= ICON_ENTRY_MAX_SIDE) {
			if (w == ICON_SZ && h == ICON_SZ) {
				have_exact = true;
				exact_off = pos + 2;
				exact_w = w;
				exact_h = h;
			} else if (w >= ICON_SZ && h >= ICON_SZ) {
				uint32_t area = w * h;

				if (!have_down || area < down_area) {
					have_down = true;
					down_off = pos + 2;
					down_w = w;
					down_h = h;
					down_area = area;
				}
			} else {
				uint32_t area = w * h;

				if (!have_up || area > up_area) {
					have_up = true;
					up_off = pos + 2;
					up_w = w;
					up_h = h;
					up_area = area;
				}
			}
		}
		/* 一辺 256 超は候補にしない（走査自体は続ける） */

		pos += 2 + entry_words;
		if (have_exact)
			break;   /* 完全一致より良い候補は無いので打ち切ってよい */
		if (pos + 2 > ctx->total_words)
			break;   /* 次のヘッダが無い */

		r = icon_read_words(ctx, pos, 2, &race);
		if (race)
			return ICON_SCAN_RACE;
		if (r == NULL)
			break;   /* これ以上読めない。ここまでの候補で確定する */
	}

	if (have_exact) {
		*sel_off = exact_off;
		*sel_w = exact_w;
		*sel_h = exact_h;
		return ICON_SCAN_OK;
	}
	if (have_down) {
		*sel_off = down_off;
		*sel_w = down_w;
		*sel_h = down_h;
		return ICON_SCAN_OK;
	}
	if (have_up) {
		*sel_off = up_off;
		*sel_w = up_w;
		*sel_h = up_h;
		return ICON_SCAN_OK;
	}
	return ICON_SCAN_NONE;
}

/*
 * 選んだエントリを 16x16 ARGB へ変換する。出力側の 1 行ぶんずつ、
 * 必要な入力行だけを GetProperty で読む（最大 256 語 = 1KB のスタック
 * バッファに収まる。§4.4.1「元の ARGB データはクライアント側に一切
 * 保持しない」の実装）。レースを検出したら *race=true にして false を返す。
 */
static bool icon_build_from_prop(struct icon_prop_ctx *ctx,
                                 uint32_t pixel_off, uint32_t sw, uint32_t sh,
                                 uint32_t *argb_out, bool *race)
{
	uint32_t row[ICON_ENTRY_MAX_SIDE];
	uint16_t dy;

	*race = false;

	for (dy = 0; dy < ICON_SZ; dy++) {
		uint32_t sy = nearest_index(dy, ICON_SZ, sh);
		xcb_get_property_reply_t *r;
		const uint32_t *src;
		uint16_t dx;

		r = icon_read_words(ctx, pixel_off + sy * sw, sw, race);
		if (*race)
			return false;
		if (r == NULL || (uint32_t)xcb_get_property_value_length(r) < sw * 4u) {
			free(r);
			return false;
		}
		src = xcb_get_property_value(r);
		memcpy(row, src, (size_t)sw * 4u);
		free(r);

		for (dx = 0; dx < ICON_SZ; dx++) {
			uint32_t sx = nearest_index(dx, ICON_SZ, sw);

			argb_out[dy * ICON_SZ + dx] = row[sx];
		}
	}
	return true;
}

/*
 * 16x16 ARGB バッファから WM 所有の icon_pix(画面深度) / icon_mask(深度1) を
 * 作る。アルファは 1bit へ量子化する（ファイル冒頭のコメント参照）。
 * 失敗時は途中で作った Pixmap も含めて何も残さず false を返す。
 */
static bool icon_pixmaps_from_argb(const uint32_t *argb,
                                   xcb_pixmap_t *out_pix, xcb_pixmap_t *out_mask)
{
	uint8_t maskbits[ICON_SZ * ICON_SZ];
	uint32_t colorbuf[ICON_SZ * ICON_SZ];
	uint32_t i;
	xcb_pixmap_t pix, mask;
	xcb_gcontext_t gc;
	uint8_t *wire;
	uint32_t wire_len;

	/* draw.c と同じ前提 (§4.4): 24/32bit TrueColor・標準 RGB マスク。
	 * それ以外の bpp は既知の制約として諦める（色が化けるより
	 * 「アイコンを描かない」方が安全）。 */
	if (icon_depth_bpp(wm.depth) != 32)
		return false;

	for (i = 0; i < ICON_SZ * ICON_SZ; i++) {
		uint8_t a = (uint8_t)(argb[i] >> 24);

		maskbits[i] = (a >= 128) ? 1u : 0u;
		colorbuf[i] = argb[i] & 0x00FFFFFFu;
	}

	pix = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, wm.depth, pix, wm.root, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
	gc = xcb_generate_id(wm.conn);
	xcb_create_gc(wm.conn, gc, pix, 0, NULL);
	xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
	             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, 0, 0, 0, wm.depth,
	             (uint32_t)sizeof(colorbuf), (const uint8_t *)colorbuf);
	xcb_free_gc(wm.conn, gc);

	wire = xy_pack(maskbits, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, &wire_len);
	if (wire == NULL) {
		xcb_free_pixmap(wm.conn, pix);
		return false;
	}

	mask = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, 1, mask, wm.root, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
	gc = xcb_generate_id(wm.conn);
	xcb_create_gc(wm.conn, gc, mask, 0, NULL);
	xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_XY_PIXMAP, mask, gc,
	             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, 0, 0, 0, 1, wire_len, wire);
	xcb_free_gc(wm.conn, gc);
	free(wire);

	*out_pix = pix;
	*out_mask = mask;
	return true;
}

/*
 * _NET_WM_ICON から 16x16 の icon_pix/icon_mask を作る。見つからなければ
 * false（out_pix/out_mask は触らない）。レース時は最初から読み直し、
 * 2 回までリトライしてそれでも駄目ならあきらめる (§4.4.1)。
 */
static bool icon_from_net_wm_icon(struct client *c,
                                  xcb_pixmap_t *out_pix, xcb_pixmap_t *out_mask)
{
	int attempt;

	for (attempt = 0; attempt <= ICON_RETRY_MAX; attempt++) {
		struct icon_prop_ctx ctx;
		uint32_t sel_off = 0, sel_w = 0, sel_h = 0;
		enum icon_scan_status st;

		ctx.win = c->win;
		ctx.total_words = 0;

		st = icon_scan(&ctx, &sel_off, &sel_w, &sel_h);
		if (st == ICON_SCAN_RACE)
			continue;
		if (st == ICON_SCAN_NONE)
			return false;

		{
			uint32_t argb[ICON_SZ * ICON_SZ];
			bool race = false;

			if (icon_build_from_prop(&ctx, sel_off, sel_w, sel_h, argb, &race)) {
				if (icon_pixmaps_from_argb(argb, out_pix, out_mask))
					return true;
				return false;   /* 変換失敗はレースではないのであきらめる */
			}
			if (!race)
				return false;
			/* race: 次の attempt で最初から読み直す */
		}
	}
	return false;   /* リトライ上限超過 (§4.4.1) */
}

/* ================================================================== *
 * WM_HINTS.icon_pixmap / icon_mask (ICCCM §4.1.2.4, SPEC §4.4.1)
 * ================================================================== */

/* 画面深度の任意サイズ(<=256角) Pixmap から 16x16 の画面深度 Pixmap を作る。
 * 既に 16x16 ならただの CopyArea、それ以外は GetImage して最近傍で
 * 拡大縮小する。失敗時は XCB_PIXMAP_NONE。 */
static xcb_pixmap_t icon_scale_colorpix(xcb_pixmap_t src, uint16_t sw, uint16_t sh)
{
	xcb_pixmap_t dst;
	xcb_gcontext_t gc;

	if (sw == 0 || sh == 0)
		return XCB_PIXMAP_NONE;

	dst = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, wm.depth, dst, wm.root, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
	gc = xcb_generate_id(wm.conn);
	xcb_create_gc(wm.conn, gc, dst, 0, NULL);

	if (sw == ICON_SZ && sh == ICON_SZ) {
		xcb_copy_area(wm.conn, src, dst, gc, 0, 0, 0, 0,
		             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
		xcb_free_gc(wm.conn, gc);
		return dst;
	}

	/* サイズが違う場合だけ GetImage で読む。sw,sh は呼び出し前提で <=256、
	 * bpp=32 前提なので一時領域は最大 256*256*4 = 256KB。Pixmap の内容は
	 * ID が変わらない限り差し替わらないので、GetProperty のような
	 * 分割読み・レース検出は不要（対象は _NET_WM_ICON のみ、§4.4.1）。
	 * GetImage の戻り値は使い終わったら即 free するので常駐しない。 */
	if (icon_depth_bpp(wm.depth) != 32) {
		xcb_free_gc(wm.conn, gc);
		xcb_free_pixmap(wm.conn, dst);
		return XCB_PIXMAP_NONE;
	}
	{
		xcb_get_image_cookie_t ck = xcb_get_image(wm.conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
		                                          src, 0, 0, sw, sh, 0xFFFFFFFFu);
		xcb_get_image_reply_t *r = xcb_get_image_reply(wm.conn, ck, NULL);
		uint32_t outbuf[ICON_SZ * ICON_SZ];
		const uint32_t *src_px;
		uint32_t data_words;
		uint16_t x, y;

		if (r == NULL) {
			xcb_free_gc(wm.conn, gc);
			xcb_free_pixmap(wm.conn, dst);
			return XCB_PIXMAP_NONE;
		}
		src_px = (const uint32_t *)(const void *)xcb_get_image_data(r);
		data_words = (uint32_t)xcb_get_image_data_length(r) / 4u;

		for (y = 0; y < ICON_SZ; y++) {
			uint32_t sy = nearest_index(y, ICON_SZ, sh);

			for (x = 0; x < ICON_SZ; x++) {
				uint32_t sx = nearest_index(x, ICON_SZ, sw);
				uint32_t idx = sy * sw + sx;

				outbuf[y * ICON_SZ + x] = (idx < data_words) ? src_px[idx] : 0u;
			}
		}
		free(r);

		xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, dst, gc,
		             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, 0, 0, 0, wm.depth,
		             (uint32_t)sizeof(outbuf), (const uint8_t *)outbuf);
	}
	xcb_free_gc(wm.conn, gc);
	return dst;
}

/* 深度1の任意サイズ(<=256角) Pixmap から 16x16 の深度1 Pixmap を作る。
 * icon_pixmap にも icon_mask にも使う共通処理。失敗時は XCB_PIXMAP_NONE。 */
static xcb_pixmap_t icon_scale_bitmap(xcb_pixmap_t src, uint16_t sw, uint16_t sh)
{
	xcb_pixmap_t dst;
	xcb_gcontext_t gc;

	if (sw == 0 || sh == 0)
		return XCB_PIXMAP_NONE;

	dst = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, 1, dst, wm.root, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
	gc = xcb_generate_id(wm.conn);
	xcb_create_gc(wm.conn, gc, dst, 0, NULL);

	if (sw == ICON_SZ && sh == ICON_SZ) {
		xcb_copy_area(wm.conn, src, dst, gc, 0, 0, 0, 0,
		             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
		xcb_free_gc(wm.conn, gc);
		return dst;
	}

	{
		const xcb_setup_t *setup = xcb_get_setup(wm.conn);
		uint32_t unit = setup->bitmap_format_scanline_unit;
		uint32_t pad = setup->bitmap_format_scanline_pad;
		bool bit_msb = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
		bool byte_msb = setup->image_byte_order == XCB_IMAGE_ORDER_MSB_FIRST;
		uint32_t stride_bits, stride_bytes;
		xcb_get_image_cookie_t ck;
		xcb_get_image_reply_t *r;
		uint8_t out[ICON_SZ * ICON_SZ];
		uint8_t *wire;
		uint32_t wire_len;
		uint16_t x, y;

		if (unit == 0)
			unit = 8;
		if (pad == 0)
			pad = 8;
		stride_bits = ((uint32_t)sw + pad - 1u) / pad * pad;
		stride_bytes = stride_bits / 8u;

		ck = xcb_get_image(wm.conn, XCB_IMAGE_FORMAT_XY_PIXMAP, src, 0, 0, sw, sh, 1);
		r = xcb_get_image_reply(wm.conn, ck, NULL);
		if (r == NULL || r->depth != 1) {
			free(r);
			xcb_free_gc(wm.conn, gc);
			xcb_free_pixmap(wm.conn, dst);
			return XCB_PIXMAP_NONE;
		}

		{
			const uint8_t *data = xcb_get_image_data(r);
			uint32_t data_len = (uint32_t)xcb_get_image_data_length(r);

			memset(out, 0, sizeof(out));
			for (y = 0; y < ICON_SZ; y++) {
				uint32_t sy = nearest_index(y, ICON_SZ, sh);

				for (x = 0; x < ICON_SZ; x++) {
					uint32_t sx = nearest_index(x, ICON_SZ, sw);
					uint32_t byte_off;
					uint8_t bit_in_byte;
					uint32_t off;

					xy_bit_pos(unit, bit_msb, byte_msb, (uint16_t)sx,
					          &byte_off, &bit_in_byte);
					if (byte_off >= stride_bytes)
						continue;
					off = sy * stride_bytes + byte_off;
					if (off < data_len && ((data[off] >> bit_in_byte) & 1u))
						out[y * ICON_SZ + x] = 1u;
				}
			}
		}
		free(r);

		wire = xy_pack(out, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, &wire_len);
		if (wire != NULL) {
			xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_XY_PIXMAP, dst, gc,
			             (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, 0, 0, 0, 1,
			             wire_len, wire);
			free(wire);
		}
		xcb_free_gc(wm.conn, gc);
		return dst;
	}
}

/* 深度1の 16x16 Pixmap を画面深度の 16x16 Pixmap へ CopyPlane で変換する。
 * ICCCM は深度1アイコンの色を規定しないため、XBM の慣習 (1=黒, 0=白) に倣う。 */
static xcb_pixmap_t icon_depth1_to_screen(xcb_pixmap_t bitmap16)
{
	xcb_pixmap_t pix;
	xcb_gcontext_t gc;
	uint32_t vals[2];

	pix = xcb_generate_id(wm.conn);
	xcb_create_pixmap(wm.conn, wm.depth, pix, wm.root, (uint16_t)ICON_SZ, (uint16_t)ICON_SZ);
	gc = xcb_generate_id(wm.conn);
	vals[0] = wm.screen->black_pixel;
	vals[1] = wm.screen->white_pixel;
	xcb_create_gc(wm.conn, gc, pix, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, vals);
	xcb_copy_plane(wm.conn, bitmap16, pix, gc, 0, 0, 0, 0,
	              (uint16_t)ICON_SZ, (uint16_t)ICON_SZ, 1);
	xcb_free_gc(wm.conn, gc);
	return pix;
}

/*
 * 深度変換・スケーリングのどちらか（または両方）が要る場合に、WM 所有の
 * icon_pix/icon_mask を組み立てる。色レイヤが作れなければ全体をあきらめる
 * （マスクだけ残っても描けないため）。
 */
static void icon_build_from_pixmap(xcb_pixmap_t src_pix, uint8_t pix_depth,
                                   uint16_t pw, uint16_t ph,
                                   xcb_pixmap_t src_mask, uint16_t mw, uint16_t mh,
                                   xcb_pixmap_t *out_pix, xcb_pixmap_t *out_mask)
{
	xcb_pixmap_t color_pix = XCB_PIXMAP_NONE;
	xcb_pixmap_t mask_pix = XCB_PIXMAP_NONE;

	if (pix_depth == 1) {
		xcb_pixmap_t scaled = icon_scale_bitmap(src_pix, pw, ph);

		if (scaled != XCB_PIXMAP_NONE) {
			color_pix = icon_depth1_to_screen(scaled);
			xcb_free_pixmap(wm.conn, scaled);
		}
	} else {
		color_pix = icon_scale_colorpix(src_pix, pw, ph);
	}

	if (color_pix == XCB_PIXMAP_NONE)
		return;   /* 色レイヤが無ければアイコン無しとして諦める */

	if (src_mask != XCB_PIXMAP_NONE)
		mask_pix = icon_scale_bitmap(src_mask, mw, mh);

	*out_pix = color_pix;
	*out_mask = mask_pix;
}

/*
 * WM_HINTS.icon_pixmap/icon_mask から icon_pix/icon_mask もしくは
 * wmh_icon_pix/wmh_icon_mask を組み立てる。どちらも見つからなければ
 * 4 つとも XCB_PIXMAP_NONE のまま（deco.c が内蔵の既定アイコンを描く）。
 */
static void icon_from_wm_hints(struct client *c,
                               xcb_pixmap_t *out_pix, xcb_pixmap_t *out_mask,
                               xcb_pixmap_t *out_wmh_pix, xcb_pixmap_t *out_wmh_mask)
{
	uint32_t *hints;
	uint32_t n = 0, flags;
	xcb_pixmap_t src_pix = XCB_PIXMAP_NONE, src_mask = XCB_PIXMAP_NONE;
	xcb_get_geometry_reply_t *pg, *mg = NULL;

	*out_pix = *out_mask = *out_wmh_pix = *out_wmh_mask = XCB_PIXMAP_NONE;

	hints = prop_get_card32_list(c->win, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, &n);
	if (hints == NULL)
		return;
	flags = hints[0];
	if ((flags & WMH_ICON_PIXMAP) && n > 3)
		src_pix = (xcb_pixmap_t)hints[3];
	if ((flags & WMH_ICON_MASK) && n > 7)
		src_mask = (xcb_pixmap_t)hints[7];
	free(hints);

	if (src_pix == XCB_PIXMAP_NONE)
		return;   /* icon_pixmap 自体が無ければ WM_HINTS 経由のアイコンは無し */

	pg = xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, src_pix), NULL);
	if (pg == NULL)
		return;   /* もう存在しない（BadDrawable） */

	if ((pg->depth != 1 && pg->depth != wm.depth) ||
	    pg->width > ICON_ENTRY_MAX_SIDE || pg->height > ICON_ENTRY_MAX_SIDE) {
		free(pg);
		return;   /* それ以外の深度は無視。サイズも防御的上限を適用 (§4.4.1) */
	}

	if (src_mask != XCB_PIXMAP_NONE) {
		mg = xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, src_mask), NULL);
		if (mg == NULL ||
		    mg->depth != 1 ||
		    mg->width > ICON_ENTRY_MAX_SIDE || mg->height > ICON_ENTRY_MAX_SIDE) {
			free(mg);
			mg = NULL;
			src_mask = XCB_PIXMAP_NONE;   /* マスクだけ諦めて色は使う */
		}
	}

	/* ゼロコピー経路: 深度・サイズともにそのまま使える時だけ、コピーを
	 * 作らずクライアントの ID を直接 wmh_icon_pix/_mask に入れる。 */
	if (pg->depth == wm.depth && pg->width == ICON_SZ && pg->height == ICON_SZ &&
	    (src_mask == XCB_PIXMAP_NONE ||
	     (mg->width == ICON_SZ && mg->height == ICON_SZ))) {
		*out_wmh_pix = src_pix;
		*out_wmh_mask = src_mask;
		free(pg);
		free(mg);
		return;
	}

	icon_build_from_pixmap(src_pix, pg->depth, pg->width, pg->height,
	                       src_mask, (uint16_t)(mg ? mg->width : 0),
	                       (uint16_t)(mg ? mg->height : 0),
	                       out_pix, out_mask);
	free(pg);
	free(mg);
}

/* ================================================================== *
 * 公開 API (w98wm.h)
 * ================================================================== */

void icon_update(struct client *c)
{
	xcb_pixmap_t new_pix = XCB_PIXMAP_NONE, new_mask = XCB_PIXMAP_NONE;
	xcb_pixmap_t new_wmh_pix = XCB_PIXMAP_NONE, new_wmh_mask = XCB_PIXMAP_NONE;

	if (c == NULL)
		return;

	/* 優先順位 (§4.4.1): _NET_WM_ICON → WM_HINTS → 内蔵既定アイコン */
	if (!icon_from_net_wm_icon(c, &new_pix, &new_mask))
		icon_from_wm_hints(c, &new_pix, &new_mask, &new_wmh_pix, &new_wmh_mask);

	/*
	 * 新しいペアを先に確定させてから古い方を解放する (§4.4.1)。頻繁に
	 * アイコンを差し替えるアプリ（進捗表示など）でもサーバ側リソースが
	 * 1 クライアントあたり Pixmap 1 枚 + マスク 1 枚を超えて単調増加しない。
	 * wmh_icon_pix/_mask はクライアント所有 ID なので Free せず、
	 * 参照を差し替えるだけにする。
	 */
	if (c->icon_pix != XCB_PIXMAP_NONE)
		xcb_free_pixmap(wm.conn, c->icon_pix);
	if (c->icon_mask != XCB_PIXMAP_NONE)
		xcb_free_pixmap(wm.conn, c->icon_mask);

	c->icon_pix = new_pix;
	c->icon_mask = new_mask;
	c->wmh_icon_pix = new_wmh_pix;
	c->wmh_icon_mask = new_wmh_mask;
}

void icon_free(struct client *c)
{
	if (c == NULL)
		return;

	if (c->icon_pix != XCB_PIXMAP_NONE) {
		xcb_free_pixmap(wm.conn, c->icon_pix);
		c->icon_pix = XCB_PIXMAP_NONE;
	}
	if (c->icon_mask != XCB_PIXMAP_NONE) {
		xcb_free_pixmap(wm.conn, c->icon_mask);
		c->icon_mask = XCB_PIXMAP_NONE;
	}

	/*
	 * wmh_icon_pix/_mask はクライアント所有の Drawable ID (w98wm.h 冒頭の
	 * コメント参照)。ここで FreePixmap すると、まだ生きている他プロセスの
	 * リソースを破壊してしまう。参照を null out するだけで解放はしない。
	 */
	c->wmh_icon_pix = XCB_PIXMAP_NONE;
	c->wmh_icon_mask = XCB_PIXMAP_NONE;
}
