/*
 * draw.h - テーマ・描画プリミティブ・フォントの契約 (Phase 2)
 *
 * 対応する仕様: SPEC §4.0〜§4.7
 * w98wm.h から include される。単独では使わない。
 */
#ifndef DRAW_H
#define DRAW_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

/* ================================================================== *
 * 配色 (SPEC §4.1)
 *
 * 既定スキーム "Windows Standard"。設定 color.<名前> で上書きできる。
 * ================================================================== */

enum {
	THEME_FACE = 0,          /* #C0C0C0 3D フェース */
	THEME_HILIGHT,           /* #FFFFFF ベベル内側の明部 */
	THEME_LIGHT,             /* #DFDFDF ベベル外側の明部 */
	THEME_SHADOW,            /* #808080 ベベル内側の暗部 */
	THEME_DKSHADOW,          /* #000000 ベベル外側の暗部 (§4.1 の訂正済み値) */
	THEME_ACTIVE_TITLE_L,    /* #000080 */
	THEME_ACTIVE_TITLE_R,    /* #1084D0 */
	THEME_INACTIVE_TITLE_L,  /* #808080 */
	THEME_INACTIVE_TITLE_R,  /* #B5B5B5 */
	THEME_TITLE_TEXT,        /* #FFFFFF */
	THEME_INACTIVE_TITLE_TEXT, /* #C0C0C0 */
	THEME_DESKTOP,           /* #008080 ティール */
	THEME_MENU_BG,           /* #C0C0C0 */
	THEME_MENU_TEXT,         /* #000000 */
	THEME_HIGHLIGHT,         /* #000080 */
	THEME_HIGHLIGHT_TEXT,    /* #FFFFFF */
	THEME_WINDOW_BG,         /* #FFFFFF */
	THEME_WINDOW_TEXT,       /* #000000 */
	THEME_DISABLED_TEXT,     /* #808080 */
	THEME_COLOR_COUNT
};

/* 設定ファイルのキー名 (color.face 等) から索引を引く。無ければ -1 */
int theme_color_index(const char *name);

/* プリセット。SPEC §11 の決定 #4 */
enum {
	THEME_PRESET_STANDARD = 0,
	THEME_PRESET_RAINY,
	THEME_PRESET_EGGPLANT,
	THEME_PRESET_PLUM,
	THEME_PRESET_HICONTRAST,
	THEME_PRESET_COUNT
};
int  theme_preset_index(const char *name);         /* 無ければ -1 */
void theme_load_preset(uint32_t *colors, int preset);  /* colors は要素数 THEME_COLOR_COUNT */

/* ================================================================== *
 * メトリクス (SPEC §4.2、96dpi 基準)
 *
 * scale 倍した実効値は theme_metrics() 経由で取る。
 * 生の定数を各所に書かないこと（HiDPI で必ず破綻する）。
 * ================================================================== */

struct metrics {
	uint16_t border_sizing;   /* 4  リサイズ可能窓のボーダー */
	uint16_t border_fixed;    /* 3  リサイズ不可窓のボーダー */
	uint16_t caption_h;       /* 18 タイトルバーの描画高 */
	uint16_t caption_h_small; /* 13 ツールウィンドウ */
	uint16_t btn_w;           /* 16 キャプションボタン */
	uint16_t btn_h;           /* 14 */
	uint16_t icon_size;       /* 16 */
	uint16_t taskbar_h;       /* 28 */
	uint16_t menu_item_h;     /* 18 */
	uint16_t menu_sep_h;      /* 7  */
	uint16_t scale;           /* 1 or 2 */
};
const struct metrics *theme_metrics(void);
void theme_init(void);          /* 設定を読んだ後に 1 回呼ぶ */

/* ================================================================== *
 * 描画プリミティブ (SPEC §4.3, §4.4)
 * ================================================================== */

/* ベベルの種別 (SPEC §4.3)。DrawEdge 相当 */
enum bevel_kind {
	BEVEL_RAISED = 0,  /* 外:light/dkshadow 内:hilight/shadow  ウィンドウ枠、ボタン */
	BEVEL_SUNKEN,      /* 外:shadow/hilight 内:dkshadow/light  入力欄 */
	BEVEL_PRESSED,     /* ボタン押下。内容を 1px 右下へずらす */
	BEVEL_BUMP         /* 1px。メニューのセパレータ、グリップ */
};

void draw_init(void);
void draw_fini(void);

/* GC は用途ごとに使い分ける。draw_* は内部で適切な GC を選ぶ */
void draw_rect(xcb_drawable_t d, int16_t x, int16_t y,
               uint16_t w, uint16_t h, uint32_t color);
void draw_line(xcb_drawable_t d, int16_t x1, int16_t y1,
               int16_t x2, int16_t y2, uint32_t color);
/* 矩形の外周に 1px 線を引く（塗らない） */
void draw_frame_rect(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h, uint32_t color);
/* ベベル。矩形の外周に沿って左上/右下の 1px 線を引く */
void draw_bevel(xcb_drawable_t d, int16_t x, int16_t y,
                uint16_t w, uint16_t h, enum bevel_kind kind);

/*
 * 水平グラデーション (SPEC §4.4)
 *
 * 固定分割ではなく「色が実際に変化する境界」で分割する。境界は
 * 3 チャネルそれぞれの変化位置の和集合であり、段数の上限は
 * ΔR+ΔG+ΔB+1（既定配色で 229、任意の色でも 766）。
 * これにより per-pixel 補間と完全に同一の結果が、幅に依存しない
 * 有界なリクエスト量で得られる。
 */
void draw_gradient_h(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h, uint32_t left, uint32_t right);

/* 1bit ビットマップを前景色で描く（キャプションボタンのグリフ等） */
void draw_glyph_bits(xcb_drawable_t d, int16_t x, int16_t y,
                     uint16_t w, uint16_t h,
                     const uint8_t *bits, uint32_t color);

/* ================================================================== *
 * フォント (SPEC §4.5)
 * ================================================================== */

/*
 * font_init(): §4.5.1 の 9 段階フォールバックを順に試す。
 *   必ず成功する（最終手段が内蔵ビットマップフォント）。
 *   失敗を fatal にしてはならない。
 * 採用結果は font_describe() で取れる（-v ログと --print-font 用）。
 */
bool font_init(const char *configured);
void font_fini(void);
const char *font_describe(void);

uint16_t font_ascent(void);
uint16_t font_height(void);

/*
 * 文字列の描画 (SPEC §4.5.4.1)
 *
 * PolyText16 を使う。ImageText16 は文字のバウンディング矩形を背景色で
 * 塗り潰すため、グラデーションのタイトルバーで使えない。
 * CJK ランの切替はこのリクエスト内のフォント切替アイテムで行う。
 * bg には触れない（呼び出し側が事前に塗る）。
 */
void font_draw(xcb_drawable_t d, int16_t x, int16_t y,
               const char *utf8, size_t len, uint32_t color);

/*
 * 幅の計測 (SPEC §4.5.2)
 *
 * 16bit フォントへの QueryFont は禁止（数百 KB の一時リプライになる）。
 * QueryTextExtents を使い、往復は最大 2 回に制限する。
 * **描画経路から呼んではならない** (§4.5.2.1)。呼び出しはタイトル変更時と
 * キャプション幅変更時のみ。
 */
uint16_t font_text_width(const char *utf8, size_t len);

/*
 * 省略表示付きの計測 (§4.5.2.1 のキャッシュ更新に使う)
 *   avail に収まる UCS-2 グリフ数を *glyphs に、末尾に "..." が要るかを
 *   *ellipsis に返す。
 */
void font_measure_fit(const char *utf8, size_t len, uint16_t avail,
                      uint8_t *glyphs, uint8_t *ellipsis);

/* ================================================================== *
 * カーソル (SPEC §4.7)
 *
 * cursor フォント (XC_*) は X11 伝統の見た目で Win98 の意匠と喧嘩するため
 * 使わない。1bit Pixmap + CreateCursor で内蔵ビットマップから作る。
 * ================================================================== */

enum {
	CURSOR_ARROW = 0,
	CURSOR_SIZE_NW, CURSOR_SIZE_N, CURSOR_SIZE_NE,
	CURSOR_SIZE_W,                  CURSOR_SIZE_E,
	CURSOR_SIZE_SW, CURSOR_SIZE_S, CURSOR_SIZE_SE,
	CURSOR_MOVE, CURSOR_WAIT,
	CURSOR_COUNT
};
void         cursor_init(void);
void         cursor_fini(void);
xcb_cursor_t cursor_get(int which);

#endif /* DRAW_H */
