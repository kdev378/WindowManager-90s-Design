/*
 * deco.c - フレームの描画と当たり判定
 *
 * 対応する仕様: docs/SPEC.md §4.0, §4.2, §4.3, §4.4, §4.4.1, §4.5.2.1, §4.7
 *
 * このファイルの原則:
 *   - 幾何は全て theme_metrics() から取る。§4.2 の数値をここへ直書きしない
 *     （scale=2 の HiDPI で必ず破綻する）。
 *   - フレーム内の位置決めは deco_layout() 1 箇所だけで行い、描画と当たり判定は
 *     同じ結果を使う。二重に計算すると「見えているボタンと押せる場所」がずれる。
 *   - 描画経路でラウンドトリップを発生させない (§3.4.1, §4.5.2.1)。
 *   - 描画経路で確保をしない。一時領域はスタックの固定長のみ。
 *   - frame の厚みは client.c の client_frame_offsets() が唯一の定義である。
 *     ここでは同じ規則（ボーダー = 固定/サイジング、上辺 = ボーダー + キャプション）
 *     を metrics 経由で再現する。値がずれたら装飾とクライアント矩形がずれる。
 */
#include <string.h>

#include <xcb/xcb.h>

#include "w98wm.h"

/* ================================================================== *
 * 1bit グリフ (SPEC §4.4)
 *
 * ビット並びは draw.c の draw_glyph_bits() の実装に合わせる:
 *   - 1 行を 8bit 単位へ切り上げてパディング（stride = (w+7)/8）
 *   - バイト内は MSB first（0x80 が左端の画素）
 * 表はすべてこの 1 箇所に固めてあるので、形式が変わってもここだけ直せばよい。
 * ================================================================== */

/* 最小化 `_`: 6x2 の横棒。ボタン内容域の下寄せ (§4.4) */
static const uint8_t glyph_min[] = {
	0xFC,  /* ###### */
	0xFC   /* ###### */
};

/* 最大化 `□`: 9x9 の箱。上辺だけ 2px 太い（Win98 のキャプション帯の見立て） */
static const uint8_t glyph_max[] = {
	0xFF, 0x80,  /* ######### */
	0xFF, 0x80,  /* ######### */
	0x80, 0x80,  /* #.......# */
	0x80, 0x80,  /* #.......# */
	0x80, 0x80,  /* #.......# */
	0x80, 0x80,  /* #.......# */
	0x80, 0x80,  /* #.......# */
	0x80, 0x80,  /* #.......# */
	0xFF, 0x80   /* ######### */
};

/* 元に戻す `❐`: 9x9。奥の箱（右上）と手前の箱（左下）が重なる (§4.4) */
static const uint8_t glyph_restore[] = {
	0x1F, 0x80,  /* ...###### */
	0x1F, 0x80,  /* ...###### */
	0x10, 0x80,  /* ...#....# */
	0xFC, 0x80,  /* ######..# */
	0xFC, 0x80,  /* ######..# */
	0x87, 0x80,  /* #....#### */
	0x84, 0x00,  /* #....#... */
	0x84, 0x00,  /* #....#... */
	0xFC, 0x00   /* ######... */
};

/* 閉じる `X`: 6x6 の斜め十字 */
static const uint8_t glyph_close[] = {
	0x84,  /* #....# */
	0x48,  /* .#..#. */
	0x30,  /* ..##.. */
	0x30,  /* ..##.. */
	0x48,  /* .#..#. */
	0x84   /* #....# */
};

/*
 * 内蔵の既定アイコン 16x16 (SPEC §4.4.1 の最終フォールバック)。
 * _NET_WM_ICON も WM_HINTS.icon_pixmap も無いクライアント用。
 */
static const uint8_t glyph_appicon[] = {
	0x00, 0x00,  /* ................ */
	0x7F, 0xFE,  /* .##############. */
	0x7F, 0xFE,  /* .##############. */
	0x7F, 0xFE,  /* .##############. */
	0x40, 0x02,  /* .#............#. */
	0x5F, 0xFA,  /* .#.##########.#. */
	0x40, 0x02,  /* .#............#. */
	0x5F, 0xE2,  /* .#.########...#. */
	0x40, 0x02,  /* .#............#. */
	0x5F, 0xFA,  /* .#.##########.#. */
	0x40, 0x02,  /* .#............#. */
	0x5F, 0x82,  /* .#.######.....#. */
	0x40, 0x02,  /* .#............#. */
	0x40, 0x02,  /* .#............#. */
	0x7F, 0xFE,  /* .##############. */
	0x00, 0x00   /* ................ */
};

#define GLYPH_MIN_W      6
#define GLYPH_MIN_H      2
#define GLYPH_BOX_W      9   /* 最大化 / 元に戻す */
#define GLYPH_BOX_H      9
#define GLYPH_CLOSE_W    6
#define GLYPH_CLOSE_H    6
#define GLYPH_APPICON_SZ 16

/* ================================================================== *
 * フレーム内レイアウト
 * ================================================================== */

enum { BTN_I_MIN = 0, BTN_I_MAX, BTN_I_CLOSE, BTN_I_COUNT };

struct deco_layout {
	int fw, fh;             /* frame の外形 */
	int border;             /* §4.2「サイジング 4px / 固定 3px」 */
	int caption;            /* §4.2「タイトルバー高 18px / ツール 13px」 */

	int cap_x, cap_y, cap_w, cap_h;   /* グラデーション帯 */

	int has_icon;
	int icon_x, icon_y, icon_sz;      /* §4.2「タイトルアイコン 16x16、左端から 2px」 */

	int btn_y, btn_w, btn_h;          /* §4.2「キャプションボタン 16x14、上下 2px マージン」 */
	int btn_x[BTN_I_COUNT];
	int btn_on[BTN_I_COUNT];          /* 表示するか */
	int btn_dis[BTN_I_COUNT];         /* 無効表示（エンボス）か */

	int text_x, text_avail;           /* タイトル文字の左端と使える幅 */
};

/*
 * §4.2 由来の固定小定数。scale 依存しない「内側の余白」だけをここに置く。
 *   CAP_PAD  : アイコンの左端インセット 2px。§4.2「タイトルアイコン … 左端から 2px」。
 *              ボタンの右端インセットと上マージンにも同じ 2px を使う
 *              （§4.2「キャプションボタン … 上下 2px マージン」と対称）。
 *   CLOSE_GAP: §4.2「ボタン間隔 0px（閉じるの左のみ 2px）」の 2px。
 *   TEXT_GAP : アイコンと文字、文字とボタンの間隔。Win98 の実測に合わせ 2px。
 */
#define CAP_PAD    2
#define CLOSE_GAP  2
#define TEXT_GAP   2

/*
 * 角の当たり判定ゾーンの一辺 (SPEC §4.7 のリサイズカーソルに対応)。
 * Win98 のボーダーは 4px しかないので、角だけで掴むのは実質不可能。
 * 実機同様、辺に沿って 16px の正方形を角として扱う。
 */
#define CORNER_ZONE 16

/* タイトルを計測する価値がある最小幅。これ未満は描画も計測もしない。 */
#define TEXT_MIN_AVAIL 4

/*
 * アイコンをマスク付きで CopyArea するための GC (§4.4.1)。
 * draw.h に「マスク付きコピー」の口が無いため、ここで 1 個だけ遅延生成して
 * 使い回す。描画のたびに作ると GC が単調増加する。
 */
static xcb_gcontext_t icon_gc = XCB_NONE;

static void deco_compute(const struct client *c, struct deco_layout *L);
static bool clip_hits(const xcb_rectangle_t *clip, int x, int y, int w, int h);
static bool clip_inside(const xcb_rectangle_t *clip, int x, int y, int w, int h);
static void draw_border(const struct client *c, const struct deco_layout *L,
                        const xcb_rectangle_t *clip);
static void draw_icon(const struct client *c, const struct deco_layout *L);
static void draw_title(struct client *c, const struct deco_layout *L);
static void draw_button(const struct client *c, const struct deco_layout *L, int i);
static void draw_emboss(xcb_drawable_t d, int x, int y, int w, int h,
                        const uint8_t *bits);
static size_t utf8_bytes_for_glyphs(const char *s, size_t len, unsigned glyphs);

/*
 * フレーム内の全部位を 1 度に決める。
 * client_frame_offsets() と同じ規則（ボーダー幅は CF_FIXED_SIZE で切り替え、
 * 上辺 = ボーダー + キャプション）を metrics 経由で再現する。
 */
static void deco_compute(const struct client *c, struct deco_layout *L)
{
	const struct metrics *m = theme_metrics();
	const struct type_props *tp = type_props(c->type);
	int dialog, n_right, x, i;

	memset(L, 0, sizeof *L);

	/* §4.2: リサイズ不可窓は固定ボーダー、それ以外はサイジングボーダー */
	L->border  = (c->flags & CF_FIXED_SIZE) ? m->border_fixed : m->border_sizing;
	/* §4.2: UTILITY 等の小キャプションは 13px、通常は 18px */
	L->caption = tp->small_caption ? m->caption_h_small : m->caption_h;

	L->fw = (int)c->geom.w + 2 * L->border;
	L->fh = (int)c->geom.h + 2 * L->border + L->caption;

	L->cap_x = L->border;
	L->cap_y = L->border;
	L->cap_w = L->fw - 2 * L->border;
	L->cap_h = L->caption;
	if (L->cap_w < 0) L->cap_w = 0;

	/*
	 * ボタンの構成 (§4.4)
	 *   DIALOG、または WM_TRANSIENT_FOR を持つ窓は [X] のみ。
	 *   [?] は実装しない（X に対応する標準の伝達手段が無い。§4.4）。
	 */
	dialog = (c->type == TYPE_DIALOG || c->transient_for != XCB_WINDOW_NONE);

	L->btn_w = m->btn_w;
	L->btn_h = m->btn_h;
	L->btn_y = L->cap_y + CAP_PAD;   /* §4.2「上下 2px マージン」 */

	L->btn_on[BTN_I_CLOSE] = 1;
	L->btn_on[BTN_I_MAX]   = !dialog;
	L->btn_on[BTN_I_MIN]   = !dialog;

	/*
	 * 無効表示 (§4.4)。_NET_WM_ALLOWED_ACTIONS に反映されるのと同じ条件で、
	 * ボタンを消さずグレイのエンボスにする。
	 *   最大化: リサイズ不可 (min==max) なら不可
	 *   最小化: タスクバーに出ない種別は最小化しても復帰手段が無いので不可
	 */
	L->btn_dis[BTN_I_MAX] = (c->flags & CF_FIXED_SIZE) ? 1 : 0;
	L->btn_dis[BTN_I_MIN] = tp->in_taskbar ? 0 : 1;

	/*
	 * 右から [X] [□] [_]。右端インセット 2px、ボタン間隔 0px、
	 * 閉じるの左だけ 2px (§4.2 の「ボタン間隔」行)。
	 *   close_x = cap_x + cap_w - 2 - btn_w
	 *   max_x   = close_x - 2 - btn_w
	 *   min_x   = max_x - btn_w
	 */
	x = L->cap_x + L->cap_w - CAP_PAD;
	n_right = L->cap_x + CAP_PAD;   /* これより左には置かない */
	for (i = BTN_I_CLOSE; i >= BTN_I_MIN; i--) {
		if (!L->btn_on[i])
			continue;
		if (i == BTN_I_MAX && L->btn_on[BTN_I_CLOSE])
			x -= CLOSE_GAP;         /* 閉じるの左のみ 2px */
		if (x - L->btn_w < n_right || L->btn_h > L->cap_h) {
			/* 幅（または小キャプションで高さ）が足りない。以降は出さない */
			for (; i >= BTN_I_MIN; i--)
				L->btn_on[i] = 0;
			break;
		}
		x -= L->btn_w;
		L->btn_x[i] = x;
	}

	/* 左端のボタン位置。文字の右限界に使う */
	for (i = BTN_I_MIN; i <= BTN_I_CLOSE; i++) {
		if (L->btn_on[i]) {
			x = L->btn_x[i];
			break;
		}
	}
	if (i > BTN_I_CLOSE)
		x = L->cap_x + L->cap_w - CAP_PAD;

	/*
	 * アイコン (§4.2「タイトルアイコン 16x16 px、左端から 2px」)。
	 * 小キャプションのように高さが足りない場合は置かない（Win98 の
	 * ツールウィンドウにもアイコンは無い）。
	 */
	L->icon_sz = m->icon_size;
	L->has_icon = (L->icon_sz + 2 <= L->cap_h && L->cap_w > L->icon_sz + 2 * CAP_PAD);
	L->icon_x = L->cap_x + CAP_PAD;
	L->icon_y = L->cap_y + (L->cap_h - L->icon_sz) / 2;   /* 18-16 → 上下 1px */

	L->text_x = L->has_icon ? L->icon_x + L->icon_sz + TEXT_GAP
	                        : L->cap_x + CAP_PAD;
	L->text_avail = x - TEXT_GAP - L->text_x;
	if (L->text_avail < 0)
		L->text_avail = 0;
}

/* clip と矩形が交差するか。clip == NULL は「全面再描画」 */
static bool clip_hits(const xcb_rectangle_t *clip, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return false;
	if (clip == NULL)
		return true;
	if (x + w <= (int)clip->x || (int)clip->x + (int)clip->width <= x)
		return false;
	if (y + h <= (int)clip->y || (int)clip->y + (int)clip->height <= y)
		return false;
	return true;
}

/* clip が矩形へ完全に収まるか（ボーダーを丸ごと省くための判定） */
static bool clip_inside(const xcb_rectangle_t *clip, int x, int y, int w, int h)
{
	if (clip == NULL)
		return false;
	return (int)clip->x >= x && (int)clip->y >= y &&
	       (int)clip->x + (int)clip->width  <= x + w &&
	       (int)clip->y + (int)clip->height <= y + h;
}

/* ================================================================== *
 * 描画
 * ================================================================== */

/*
 * ボーダー (SPEC §4.3)。外→内のピクセル並びは
 *   1px: TL=light   BR=dkshadow   ← 外側ベベル
 *   1px: TL=hilight BR=shadow     ← 内側ベベル
 *   2px: face                     ← フェース
 * 外側と内側の 2 本は BEVEL_RAISED が 1 回で引く（§4.3 の表）。
 * 残りの face はここで塗る（固定ボーダー 3px なら 1px、サイジング 4px なら 2px）。
 */
static void draw_border(const struct client *c, const struct deco_layout *L,
                        const xcb_rectangle_t *clip)
{
	uint32_t face = wm.cfg.color[THEME_FACE];
	int fb = L->border - 2;          /* face の厚み */
	int inner_w = L->fw - 4, inner_h = L->fh - 4;

	draw_bevel(c->frame, 0, 0, (uint16_t)L->fw, (uint16_t)L->fh, BEVEL_RAISED);

	if (fb <= 0 || inner_w <= 0 || inner_h <= 0)
		return;

	/* 上・下・左・右の 4 本で face のリングを作る */
	if (clip_hits(clip, 2, 2, inner_w, fb))
		draw_rect(c->frame, 2, 2, (uint16_t)inner_w, (uint16_t)fb, face);
	if (clip_hits(clip, 2, L->fh - 2 - fb, inner_w, fb))
		draw_rect(c->frame, 2, (int16_t)(L->fh - 2 - fb),
		          (uint16_t)inner_w, (uint16_t)fb, face);
	if (inner_h - 2 * fb > 0) {
		if (clip_hits(clip, 2, 2 + fb, fb, inner_h - 2 * fb))
			draw_rect(c->frame, 2, (int16_t)(2 + fb),
			          (uint16_t)fb, (uint16_t)(inner_h - 2 * fb), face);
		if (clip_hits(clip, L->fw - 2 - fb, 2 + fb, fb, inner_h - 2 * fb))
			draw_rect(c->frame, (int16_t)(L->fw - 2 - fb), (int16_t)(2 + fb),
			          (uint16_t)fb, (uint16_t)(inner_h - 2 * fb), face);
	}
}

/*
 * アイコン (SPEC §4.4.1)。
 * icon_pix / icon_mask は WM 所有の 16x16、wmh_icon_pix / _mask は
 * WM_HINTS 由来のクライアント所有 ID。ここでは **描くだけ** で、
 * どちらも解放しない（他プロセスの資源を FreePixmap すると相手が壊れる）。
 * icccm.c が深度 1 なら CopyPlane、画面深度なら CopyArea で 16x16 の
 * icon_pix を作る規定なので、ここへ来る時点で常に画面深度である。
 */
static void draw_icon(const struct client *c, const struct deco_layout *L)
{
	xcb_pixmap_t pix = c->icon_pix != XCB_PIXMAP_NONE ? c->icon_pix : c->wmh_icon_pix;
	xcb_pixmap_t msk = c->icon_pix != XCB_PIXMAP_NONE ? c->icon_mask : c->wmh_icon_mask;
	xcb_gcontext_t gc;
	uint32_t vals[3];

	if (pix == XCB_PIXMAP_NONE) {
		/* §4.4.1 の最終フォールバック。内蔵の既定アイコン */
		int gx = L->icon_x + (L->icon_sz - GLYPH_APPICON_SZ) / 2;
		int gy = L->icon_y + (L->icon_sz - GLYPH_APPICON_SZ) / 2;
		draw_glyph_bits(c->frame, (int16_t)gx, (int16_t)gy,
		                GLYPH_APPICON_SZ, GLYPH_APPICON_SZ,
		                glyph_appicon, wm.cfg.color[THEME_DKSHADOW]);
		return;
	}

	/* マスク付きの CopyArea (§4.4.1)。GC は 1 個を遅延生成して使い回す */
	if (icon_gc == XCB_NONE) {
		icon_gc = xcb_generate_id(wm.conn);
		vals[0] = 0;
		xcb_create_gc(wm.conn, icon_gc, wm.root, XCB_GC_GRAPHICS_EXPOSURES, vals);
	}
	gc = icon_gc;

	if (msk != XCB_PIXMAP_NONE) {
		/*
		 * **値リストはマスクのビット値の昇順**で並べること。
		 *   CLIP_ORIGIN_X(0x20000) < CLIP_ORIGIN_Y(0x40000) < CLIP_MASK(0x80000)
		 * マスクに書いた順ではない。ここを取り違えると、サーバは
		 * CLIP_MASK として icon_y の値を Pixmap ID と解釈し、
		 * BadPixmap を出しながらアイコンのクリップも効かなくなる
		 * （実際にこの誤りで毎回 BadPixmap が出ていた）。
		 * XSync のアラーム属性でも同じ誤りをしたので、xcb の値リストは
		 * 常にビット値順であることを意識すること。
		 */
		vals[0] = (uint32_t)(int32_t)L->icon_x;   /* CLIP_ORIGIN_X */
		vals[1] = (uint32_t)(int32_t)L->icon_y;   /* CLIP_ORIGIN_Y */
		vals[2] = msk;                            /* CLIP_MASK     */
		xcb_change_gc(wm.conn, gc,
		              XCB_GC_CLIP_ORIGIN_X | XCB_GC_CLIP_ORIGIN_Y | XCB_GC_CLIP_MASK,
		              vals);
	}
	xcb_copy_area(wm.conn, pix, c->frame, gc, 0, 0,
	              (int16_t)L->icon_x, (int16_t)L->icon_y,
	              (uint16_t)L->icon_sz, (uint16_t)L->icon_sz);
	if (msk != XCB_PIXMAP_NONE) {
		vals[0] = XCB_NONE;
		xcb_change_gc(wm.conn, gc, XCB_GC_CLIP_MASK, vals);
	}
}

/*
 * タイトル文字 (SPEC §4.4, §4.5.2.1)
 *
 * **描画経路で計測してはならない。** ライブ移動でウィンドウを他の窓の上に
 * 重ねてドラッグすると、下にある窓の露出領域ごとに Expose が飛ぶ。
 * 60Hz のドラッグでは下の窓のタイトルが毎フレーム再描画されるので、
 * ここで font_text_width() / font_measure_fit() を呼ぶと 30ms RTT の
 * リモート X で毎秒数百回の同期往復になり、画面全体が固まる。
 * これは §2.3.1 で撤回した「描画のたびにプロパティを読む」設計と同型の欠陥で、
 * §4.5.2.1 はまさにこれを防ぐために書かれている。
 *
 * したがって使うのは c->draw_glyphs / c->draw_ellipsis のキャッシュだけ。
 * 計測をやり直すのはキャッシュが古い時に限る:
 *   - c->caption_w_at_measure が現在の文字領域幅と違う（リサイズ・装飾変更）
 *   - draw_glyphs == 0 なのにタイトルが空でない（icccm.c のタイトル変更通知）
 *
 * caption_w_at_measure に入れるのは「キャプション帯の幅」ではなく
 * 「アイコンとボタンを除いた、文字に使える幅」＝ font_measure_fit() へ
 * 渡した avail そのものである。ボタン構成（ダイアログ化・小キャプション化）が
 * 変わると帯幅が同じでも avail は変わるため、avail を鍵にしないと
 * 古い省略位置を描き続ける。他モジュールがキャッシュを捨てる時は
 * draw_glyphs = 0 にすること（caption_w_at_measure を触る必要は無い）。
 */
static void draw_title(struct client *c, const struct deco_layout *L)
{
	char buf[WM_TITLE_MAX + 4];
	uint32_t color;
	size_t nb;
	int base_y, fh;
	uint16_t avail = (uint16_t)L->text_avail;

	if (c->title_len == 0 || L->text_avail < TEXT_MIN_AVAIL)
		return;

	if (c->caption_w_at_measure != avail ||
	    (c->draw_glyphs == 0 && c->title_len > 0)) {
		uint8_t glyphs = 0, ell = 0;

		font_measure_fit(c->title, c->title_len, avail, &glyphs, &ell);
		if (glyphs == 0)
			ell = 1;          /* 1 文字も入らないなら "..." だけ出す */
		c->draw_glyphs = glyphs;
		c->draw_ellipsis = ell;
		c->caption_w_at_measure = avail;
	}

	nb = utf8_bytes_for_glyphs(c->title, c->title_len, c->draw_glyphs);
	if (nb > sizeof buf - 4)
		nb = sizeof buf - 4;
	memcpy(buf, c->title, nb);
	if (c->draw_ellipsis) {
		buf[nb++] = '.';
		buf[nb++] = '.';
		buf[nb++] = '.';
	}
	if (nb == 0)
		return;

	/*
	 * ベースライン。キャプション帯の中央にフォントの箱を置く。
	 * 省略記号は別リクエストにせず 1 本の文字列にまとめる（往復も
	 * リクエスト数も増やさないため）。
	 */
	fh = (int)font_height();
	base_y = L->cap_y + (L->cap_h - fh) / 2 + (int)font_ascent();
	if (fh > L->cap_h)
		base_y = L->cap_y + (int)font_ascent();

	color = (c->states & ST_FOCUSED) ? wm.cfg.color[THEME_TITLE_TEXT]
	                                 : wm.cfg.color[THEME_INACTIVE_TITLE_TEXT];
	font_draw(c->frame, (int16_t)L->text_x, (int16_t)base_y, buf, nb, color);
}

/* 無効表示 (SPEC §4.1「disabled_text は #FFFFFF の 1px オフセット付き」) */
static void draw_emboss(xcb_drawable_t d, int x, int y, int w, int h,
                        const uint8_t *bits)
{
	draw_glyph_bits(d, (int16_t)(x + 1), (int16_t)(y + 1),
	                (uint16_t)w, (uint16_t)h, bits, wm.cfg.color[THEME_HILIGHT]);
	draw_glyph_bits(d, (int16_t)x, (int16_t)y,
	                (uint16_t)w, (uint16_t)h, bits, wm.cfg.color[THEME_DISABLED_TEXT]);
}

/*
 * キャプションボタン 1 個 (SPEC §4.3, §4.4)
 *
 * 押下表示は「押している最中で、かつポインタがまだそのボタンの上にある」時だけ。
 * Windows は押したままボタン外へ出ると押下表示が解除される（そこで離しても
 * 動作しない）ので、その挙動を再現する。
 */
static void draw_button(const struct client *c, const struct deco_layout *L, int i)
{
	static const enum frame_part part_of[BTN_I_COUNT] = {
		PART_BTN_MIN, PART_BTN_MAX, PART_BTN_CLOSE
	};
	const uint8_t *bits;
	int gw, gh, gx, gy, bx = L->btn_x[i], by = L->btn_y, pressed, off;

	pressed = (c->press_part == (uint8_t)part_of[i] &&
	           c->hover_part == (uint8_t)part_of[i]);
	off = pressed ? 1 : 0;   /* §4.3: 押下は内容を 1px 右下へずらす */

	draw_rect(c->frame, (int16_t)bx, (int16_t)by,
	          (uint16_t)L->btn_w, (uint16_t)L->btn_h, wm.cfg.color[THEME_FACE]);
	draw_bevel(c->frame, (int16_t)bx, (int16_t)by,
	           (uint16_t)L->btn_w, (uint16_t)L->btn_h,
	           pressed ? BEVEL_PRESSED : BEVEL_RAISED);

	switch (i) {
	case BTN_I_MIN:
		bits = glyph_min;
		gw = GLYPH_MIN_W;
		gh = GLYPH_MIN_H;
		/* 内容域（ベベル 2px を除いた 12x10）の下寄せ。下に 1px 余白 */
		gx = bx + (L->btn_w - gw) / 2;
		gy = by + L->btn_h - 2 - 1 - gh;
		break;
	case BTN_I_MAX:
		/* 最大化中は `❐`（元に戻す）を出す (§4.4) */
		bits = (c->states & ST_MAXIMIZED) == ST_MAXIMIZED ? glyph_restore : glyph_max;
		gw = GLYPH_BOX_W;
		gh = GLYPH_BOX_H;
		gx = bx + (L->btn_w - gw) / 2;
		gy = by + (L->btn_h - gh) / 2;
		break;
	default:
		bits = glyph_close;
		gw = GLYPH_CLOSE_W;
		gh = GLYPH_CLOSE_H;
		gx = bx + (L->btn_w - gw) / 2;
		gy = by + (L->btn_h - gh) / 2;
		break;
	}

	if (L->btn_dis[i]) {
		/* 無効でもボタンは消さずグレイのエンボスにする (§4.4) */
		draw_emboss(c->frame, gx, gy, gw, gh, bits);
		return;
	}
	draw_glyph_bits(c->frame, (int16_t)(gx + off), (int16_t)(gy + off),
	                (uint16_t)gw, (uint16_t)gh, bits,
	                wm.cfg.color[THEME_DKSHADOW]);
}

/* ================================================================== *
 * 公開関数
 * ================================================================== */

void deco_draw(struct client *c, const xcb_rectangle_t *clip)
{
	struct deco_layout L;
	uint32_t left, right;
	int gy, gh, i, cap_touched;

	if (c == NULL || c->frame == XCB_WINDOW_NONE)
		return;
	if ((c->flags & CF_DECORATED) == 0)
		return;

	deco_compute(c, &L);
	if (L.fw <= 0 || L.fh <= 0)
		return;

	/* ボーダーは clip が内側矩形へ完全に収まっている時だけ省ける */
	if (!clip_inside(clip, L.border, L.border,
	                 L.fw - 2 * L.border, L.fh - 2 * L.border))
		draw_border(c, &L, clip);

	cap_touched = clip_hits(clip, L.cap_x, L.cap_y, L.cap_w, L.cap_h);
	if (!cap_touched)
		return;

	/*
	 * キャプション背景 (SPEC §4.4)。左→右の水平グラデーション。
	 * 横方向は必ず全幅で描く。x/w を clip で狭めると補間の基準が変わり、
	 * §4.0 が要求するピクセル一致が崩れるため。縦方向だけは色に影響しない
	 * ので clip で切り詰めてよい（ドラッグ中の再描画量が減る）。
	 */
	left  = (c->states & ST_FOCUSED) ? wm.cfg.color[THEME_ACTIVE_TITLE_L]
	                                 : wm.cfg.color[THEME_INACTIVE_TITLE_L];
	right = (c->states & ST_FOCUSED) ? wm.cfg.color[THEME_ACTIVE_TITLE_R]
	                                 : wm.cfg.color[THEME_INACTIVE_TITLE_R];
	gy = L.cap_y;
	gh = L.cap_h;
	if (clip != NULL) {
		int top = (int)clip->y > gy ? (int)clip->y : gy;
		int bot = (int)clip->y + (int)clip->height;
		if (bot > gy + gh) bot = gy + gh;
		gy = top;
		gh = bot - top;
	}
	if (gh > 0 && L.cap_w > 0)
		draw_gradient_h(c->frame, (int16_t)L.cap_x, (int16_t)gy,
		                (uint16_t)L.cap_w, (uint16_t)gh, left, right);

	/*
	 * 背景を塗った以上、その上に載る要素は clip の内外にかかわらず描き直す。
	 * （グラデーションは横方向を切れないので、キャプションに触れた時点で
	 *   帯全体が塗り直されている）
	 */
	if (L.has_icon)
		draw_icon(c, &L);

	draw_title(c, &L);

	for (i = BTN_I_MIN; i <= BTN_I_CLOSE; i++) {
		if (L.btn_on[i])
			draw_button(c, &L, i);
	}
}

/*
 * 全面を dirty にする。
 * 方針は「自分で描かず、サーバに Expose を出させる」に統一する。
 * ここで直接 deco_draw() を呼ぶと、直後に届く Expose で二重描画になり、
 * さらに描画とイベント処理の順序がフレームごとに変わって再現性が落ちる。
 * xcb_clear_area() の exposures=1 が唯一の起点。
 */
void deco_invalidate(struct client *c)
{
	if (c == NULL || c->frame == XCB_WINDOW_NONE)
		return;
	if ((c->flags & CF_DECORATED) == 0)
		return;
	/* w=h=0 は「ウィンドウの端まで」の意味 (X プロトコル ClearArea) */
	xcb_clear_area(wm.conn, 1, c->frame, 0, 0, 0, 0);
}

/*
 * frame 相対座標 → 部位 (SPEC §4.4, §4.6)
 *
 * 部位ごとのサブウィンドウは作らない (§3.1) ので、ここが唯一の判定である。
 * 描画と同じ deco_compute() を使うため、見えているボタンと押せる場所は必ず一致する。
 */
enum frame_part deco_hit_test(const struct client *c, int16_t fx, int16_t fy)
{
	struct deco_layout L;
	int x = fx, y = fy, corner, i;
	int in_l, in_r, in_t, in_b;

	if (c == NULL || (c->flags & CF_DECORATED) == 0)
		return PART_NONE;

	deco_compute(c, &L);
	if (x < 0 || y < 0 || x >= L.fw || y >= L.fh)
		return PART_NONE;

	in_l = x < L.border;
	in_r = x >= L.fw - L.border;
	in_t = y < L.border;
	in_b = y >= L.fh - L.border;

	if (in_l || in_r || in_t || in_b) {
		/* 角のゾーンは辺より優先。ボーダーが 4px しかないので
		 * 実機同様、辺に沿った 16px 四方を角として扱う */
		corner = CORNER_ZONE;
		if (corner > L.fw / 2) corner = L.fw / 2;
		if (corner > L.fh / 2) corner = L.fh / 2;

		if (in_t) {
			if (x < corner) return PART_BORDER_NW;
			if (x >= L.fw - corner) return PART_BORDER_NE;
			return PART_BORDER_N;
		}
		if (in_b) {
			if (x < corner) return PART_BORDER_SW;
			if (x >= L.fw - corner) return PART_BORDER_SE;
			return PART_BORDER_S;
		}
		if (in_l) {
			if (y < corner) return PART_BORDER_NW;
			if (y >= L.fh - corner) return PART_BORDER_SW;
			return PART_BORDER_W;
		}
		if (y < corner) return PART_BORDER_NE;
		if (y >= L.fh - corner) return PART_BORDER_SE;
		return PART_BORDER_E;
	}

	/* キャプション帯 */
	if (y < L.cap_y + L.cap_h) {
		static const enum frame_part part_of[BTN_I_COUNT] = {
			PART_BTN_MIN, PART_BTN_MAX, PART_BTN_CLOSE
		};
		for (i = BTN_I_MIN; i <= BTN_I_CLOSE; i++) {
			if (!L.btn_on[i])
				continue;
			if (x >= L.btn_x[i] && x < L.btn_x[i] + L.btn_w &&
			    y >= L.btn_y && y < L.btn_y + L.btn_h)
				return part_of[i];
		}
		/*
		 * アイコンはシステムメニューのホットスポット (§4.6)。
		 * 左の 2px マージンも含めて帯の高さいっぱいを判定に使う
		 * （16x16 ちょうどだと 1px 外して掴み損ねる）。
		 */
		if (L.has_icon && x < L.icon_x + L.icon_sz)
			return PART_ICON;
		return PART_TITLE;
	}

	return PART_CLIENT;
}

/* 部位 → リサイズ辺 (EDGE_* の和)。ボーダー以外は 0 */
uint8_t deco_part_edge(enum frame_part p)
{
	switch (p) {
	case PART_BORDER_N:  return EDGE_T;
	case PART_BORDER_S:  return EDGE_B;
	case PART_BORDER_W:  return EDGE_L;
	case PART_BORDER_E:  return EDGE_R;
	case PART_BORDER_NW: return EDGE_T | EDGE_L;
	case PART_BORDER_NE: return EDGE_T | EDGE_R;
	case PART_BORDER_SW: return EDGE_B | EDGE_L;
	case PART_BORDER_SE: return EDGE_B | EDGE_R;
	default:             return 0;
	}
}

/* 部位 → カーソル (SPEC §4.7) */
int deco_part_cursor(enum frame_part p)
{
	switch (p) {
	case PART_BORDER_N:  return CURSOR_SIZE_N;
	case PART_BORDER_S:  return CURSOR_SIZE_S;
	case PART_BORDER_W:  return CURSOR_SIZE_W;
	case PART_BORDER_E:  return CURSOR_SIZE_E;
	case PART_BORDER_NW: return CURSOR_SIZE_NW;
	case PART_BORDER_NE: return CURSOR_SIZE_NE;
	case PART_BORDER_SW: return CURSOR_SIZE_SW;
	case PART_BORDER_SE: return CURSOR_SIZE_SE;
	default:             return CURSOR_ARROW;
	}
}

/*
 * frame へカーソルを割り当てる (SPEC §4.7)。
 *
 * ポインタは大量の MotionNotify を出すので、実際に形が変わる時だけ
 * ChangeWindowAttributes を投げる。ポインタは同時に 1 つの frame の上にしか
 * いないため、直前の (frame, cursor) を 1 組だけ覚えれば足りる。
 * struct client には保存場所が無く、ヘッダは変更できないためここに置く。
 */
void deco_set_cursor(struct client *c, enum frame_part p)
{
	static xcb_window_t last_frame = XCB_WINDOW_NONE;
	static int last_cursor = -1;
	uint32_t vals[1];
	int which;

	if (c == NULL || c->frame == XCB_WINDOW_NONE)
		return;

	which = deco_part_cursor(p);
	/* リサイズ不可の窓ではボーダーにリサイズカーソルを出さない
	 * （掴んでも動かないカーソルは嘘になる。§3.4） */
	if ((c->flags & CF_FIXED_SIZE) && deco_part_edge(p) != 0)
		which = CURSOR_ARROW;

	if (c->frame == last_frame && which == last_cursor)
		return;

	vals[0] = cursor_get(which);
	xcb_change_window_attributes(wm.conn, c->frame, XCB_CW_CURSOR, vals);
	last_frame = c->frame;
	last_cursor = which;
}

/* ================================================================== *
 * 小物
 * ================================================================== */

/*
 * UCS-2 グリフ数 → UTF-8 バイト数 (SPEC §2.3.1)
 *
 * font_measure_fit() が返すのは「描画するグリフ数」だが font_draw() が
 * 取るのはバイト長なので、ここで橋渡しする。utf8_to_ucs2() と同じく
 * 1 コードポイント = 1 グリフ（BMP 外は U+FFFD 1 個）として数える。
 * 不正なシーケンスでも必ず 1 バイト以上進める（無限ループ防止）。
 * 往復もメモリ確保も発生しない純粋な走査なので描画経路で呼んでよい。
 */
static size_t utf8_bytes_for_glyphs(const char *s, size_t len, unsigned glyphs)
{
	size_t i = 0;
	unsigned n = 0;

	while (i < len && n < glyphs) {
		unsigned char b = (unsigned char)s[i];
		size_t adv;

		if (b < 0x80u)             adv = 1;
		else if ((b & 0xE0u) == 0xC0u) adv = 2;
		else if ((b & 0xF0u) == 0xE0u) adv = 3;
		else if ((b & 0xF8u) == 0xF0u) adv = 4;
		else                       adv = 1;   /* 不正な先頭バイト */

		if (i + adv > len)
			adv = len - i;
		i += adv;
		n++;
	}
	return i;
}
