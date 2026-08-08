/*
 * theme.c - 配色テーブルとメトリクス (SPEC §4.1, §4.2)
 *
 * 色は 0x00RRGGBB として保持する（config.c の #RRGGBB パースと同じ表現）。
 * draw.c はこの値をそのまま GC の foreground pixel として使う。
 * 本プロジェクトは XRender/cairo を使わず、実質すべての現代的な X サーバが
 * 24/32bit TrueColor で標準 RGB マスク (0xFF0000/0x00FF00/0x0000FF) を
 * 持つことを前提にしている（他モジュールにも Colormap 変換の処理が
 * 存在しないため、この前提はプロジェクト全体の設計判断である）。
 */
#include "w98wm.h"

/* ================================================================== *
 * 配色 (SPEC §4.1)
 * ================================================================== */

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/*
 * "Windows Standard" 既定スキーム。SPEC §4.1 の表そのもの。
 * THEME_DKSHADOW は #000000（§4.1 で訂正済み。#0A0A0A は Web 実装(98.css)の
 * 意匠的選択であり実機の値ではないため、ここで #0A0A0A に「戻さない」こと）。
 */
static const uint32_t THEME_STANDARD[THEME_COLOR_COUNT] = {
	[THEME_FACE]                = RGB(0xC0, 0xC0, 0xC0),
	[THEME_HILIGHT]              = RGB(0xFF, 0xFF, 0xFF),
	[THEME_LIGHT]                = RGB(0xDF, 0xDF, 0xDF),
	[THEME_SHADOW]               = RGB(0x80, 0x80, 0x80),
	[THEME_DKSHADOW]             = RGB(0x00, 0x00, 0x00),
	[THEME_ACTIVE_TITLE_L]       = RGB(0x00, 0x00, 0x80),
	[THEME_ACTIVE_TITLE_R]       = RGB(0x10, 0x84, 0xD0),
	[THEME_INACTIVE_TITLE_L]     = RGB(0x80, 0x80, 0x80),
	[THEME_INACTIVE_TITLE_R]     = RGB(0xB5, 0xB5, 0xB5),
	[THEME_TITLE_TEXT]           = RGB(0xFF, 0xFF, 0xFF),
	[THEME_INACTIVE_TITLE_TEXT]  = RGB(0xC0, 0xC0, 0xC0),
	[THEME_DESKTOP]              = RGB(0x00, 0x80, 0x80),
	[THEME_MENU_BG]              = RGB(0xC0, 0xC0, 0xC0),
	[THEME_MENU_TEXT]            = RGB(0x00, 0x00, 0x00),
	[THEME_HIGHLIGHT]            = RGB(0x00, 0x00, 0x80),
	[THEME_HIGHLIGHT_TEXT]       = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_BG]            = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_TEXT]          = RGB(0x00, 0x00, 0x00),
	[THEME_DISABLED_TEXT]        = RGB(0x80, 0x80, 0x80),
};

/*
 * 以下 4 プリセットは SPEC §4.1 に表が無い（"Win98 同梱スキームを数点同梱する"
 * とだけ書かれている）。実機 Win98 の Rainy Day / Eggplant / Plum /
 * High Contrast Black の正確な RGB 値は、この場で参照可能な一次資料が無く
 * 確認できていない。値には次の方針を取った:
 *
 *   - Rainy Day / Eggplant / Plum は、当時の Win98 同梱スキームの多くが
 *     3D 部品（face/hilight/light/shadow/dkshadow）を標準スキームのまま
 *     残し、タイトルバーとデスクトップ背景だけを配色していた記憶に基づき、
 *     3D 部品は標準値を流用した。これ自体が未検証の一般化である。
 *   - High Contrast Black のみ、アクセシビリティ用スキームは全要素を
 *     塗り替えるのが通例（3D 部品も含む）という理解に基づき、全面的に
 *     黒地・白文字・黄色ハイライトへ作り替えた。
 *
 * **これらは全て未検証の推測値であり、§4.0 の基準スクリーンショットで
 * 実機と突き合わせて確定させる必要がある。** 数値に見せかけた精度を
 * 主張しないよう、ここに明記する。
 */

/* Rainy Day: 灰青系のタイトルバー。未検証 (上記参照) */
static const uint32_t THEME_RAINY[THEME_COLOR_COUNT] = {
	[THEME_FACE]                = RGB(0xC0, 0xC0, 0xC0),
	[THEME_HILIGHT]              = RGB(0xFF, 0xFF, 0xFF),
	[THEME_LIGHT]                = RGB(0xDF, 0xDF, 0xDF),
	[THEME_SHADOW]               = RGB(0x80, 0x80, 0x80),
	[THEME_DKSHADOW]             = RGB(0x00, 0x00, 0x00),
	[THEME_ACTIVE_TITLE_L]       = RGB(0x42, 0x63, 0x8C), /* 未検証: 灰青 */
	[THEME_ACTIVE_TITLE_R]       = RGB(0x8C, 0xA8, 0xC8), /* 未検証: 灰青(明) */
	[THEME_INACTIVE_TITLE_L]     = RGB(0x80, 0x80, 0x80),
	[THEME_INACTIVE_TITLE_R]     = RGB(0xB5, 0xB5, 0xB5),
	[THEME_TITLE_TEXT]           = RGB(0xFF, 0xFF, 0xFF),
	[THEME_INACTIVE_TITLE_TEXT]  = RGB(0xC0, 0xC0, 0xC0),
	[THEME_DESKTOP]              = RGB(0x63, 0x65, 0x6A), /* 未検証: 雨曇りの灰 */
	[THEME_MENU_BG]              = RGB(0xC0, 0xC0, 0xC0),
	[THEME_MENU_TEXT]            = RGB(0x00, 0x00, 0x00),
	[THEME_HIGHLIGHT]            = RGB(0x42, 0x63, 0x8C), /* 未検証 */
	[THEME_HIGHLIGHT_TEXT]       = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_BG]            = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_TEXT]          = RGB(0x00, 0x00, 0x00),
	[THEME_DISABLED_TEXT]        = RGB(0x80, 0x80, 0x80),
};

/* Eggplant: 紫のタイトルバー。未検証 (上記参照) */
static const uint32_t THEME_EGGPLANT[THEME_COLOR_COUNT] = {
	[THEME_FACE]                = RGB(0xC0, 0xC0, 0xC0),
	[THEME_HILIGHT]              = RGB(0xFF, 0xFF, 0xFF),
	[THEME_LIGHT]                = RGB(0xDF, 0xDF, 0xDF),
	[THEME_SHADOW]               = RGB(0x80, 0x80, 0x80),
	[THEME_DKSHADOW]             = RGB(0x00, 0x00, 0x00),
	[THEME_ACTIVE_TITLE_L]       = RGB(0x55, 0x00, 0x55), /* 未検証: 茄子紫 */
	[THEME_ACTIVE_TITLE_R]       = RGB(0x94, 0x4F, 0x94), /* 未検証: 茄子紫(明) */
	[THEME_INACTIVE_TITLE_L]     = RGB(0x80, 0x80, 0x80),
	[THEME_INACTIVE_TITLE_R]     = RGB(0xB5, 0xB5, 0xB5),
	[THEME_TITLE_TEXT]           = RGB(0xFF, 0xFF, 0xFF),
	[THEME_INACTIVE_TITLE_TEXT]  = RGB(0xC0, 0xC0, 0xC0),
	[THEME_DESKTOP]              = RGB(0x40, 0x00, 0x40), /* 未検証: 濃紺紫 */
	[THEME_MENU_BG]              = RGB(0xC0, 0xC0, 0xC0),
	[THEME_MENU_TEXT]            = RGB(0x00, 0x00, 0x00),
	[THEME_HIGHLIGHT]            = RGB(0x55, 0x00, 0x55), /* 未検証 */
	[THEME_HIGHLIGHT_TEXT]       = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_BG]            = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_TEXT]          = RGB(0x00, 0x00, 0x00),
	[THEME_DISABLED_TEXT]        = RGB(0x80, 0x80, 0x80),
};

/* Plum: 赤紫（プラム色）のタイトルバー。未検証 (上記参照) */
static const uint32_t THEME_PLUM[THEME_COLOR_COUNT] = {
	[THEME_FACE]                = RGB(0xC0, 0xC0, 0xC0),
	[THEME_HILIGHT]              = RGB(0xFF, 0xFF, 0xFF),
	[THEME_LIGHT]                = RGB(0xDF, 0xDF, 0xDF),
	[THEME_SHADOW]               = RGB(0x80, 0x80, 0x80),
	[THEME_DKSHADOW]             = RGB(0x00, 0x00, 0x00),
	[THEME_ACTIVE_TITLE_L]       = RGB(0x84, 0x2A, 0x5A), /* 未検証: プラム色 */
	[THEME_ACTIVE_TITLE_R]       = RGB(0xC8, 0x84, 0xA8), /* 未検証: プラム色(明) */
	[THEME_INACTIVE_TITLE_L]     = RGB(0x80, 0x80, 0x80),
	[THEME_INACTIVE_TITLE_R]     = RGB(0xB5, 0xB5, 0xB5),
	[THEME_TITLE_TEXT]           = RGB(0xFF, 0xFF, 0xFF),
	[THEME_INACTIVE_TITLE_TEXT]  = RGB(0xC0, 0xC0, 0xC0),
	[THEME_DESKTOP]              = RGB(0x52, 0x1A, 0x38), /* 未検証: 暗いプラム */
	[THEME_MENU_BG]              = RGB(0xC0, 0xC0, 0xC0),
	[THEME_MENU_TEXT]            = RGB(0x00, 0x00, 0x00),
	[THEME_HIGHLIGHT]            = RGB(0x84, 0x2A, 0x5A), /* 未検証 */
	[THEME_HIGHLIGHT_TEXT]       = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_BG]            = RGB(0xFF, 0xFF, 0xFF),
	[THEME_WINDOW_TEXT]          = RGB(0x00, 0x00, 0x00),
	[THEME_DISABLED_TEXT]        = RGB(0x80, 0x80, 0x80),
};

/*
 * High Contrast Black: アクセシビリティ用。黒地・白文字・黄色の選択強調。
 * 未検証 (上記参照)。特にハイライト色を黄色としたのは一般的な
 * ハイコントラスト配色の慣習に基づく推測であり、実機値ではない。
 */
static const uint32_t THEME_HICONTRAST[THEME_COLOR_COUNT] = {
	[THEME_FACE]                = RGB(0x00, 0x00, 0x00),
	[THEME_HILIGHT]              = RGB(0xFF, 0xFF, 0xFF),
	[THEME_LIGHT]                = RGB(0xFF, 0xFF, 0xFF),
	[THEME_SHADOW]               = RGB(0x80, 0x80, 0x80),
	[THEME_DKSHADOW]             = RGB(0xFF, 0xFF, 0xFF), /* 未検証: 黒地では白で縁取り */
	[THEME_ACTIVE_TITLE_L]       = RGB(0x00, 0x00, 0x00),
	[THEME_ACTIVE_TITLE_R]       = RGB(0x00, 0x00, 0x00), /* 単色（グラデーション無し） */
	[THEME_INACTIVE_TITLE_L]     = RGB(0x00, 0x00, 0x00),
	[THEME_INACTIVE_TITLE_R]     = RGB(0x00, 0x00, 0x00),
	[THEME_TITLE_TEXT]           = RGB(0xFF, 0xFF, 0xFF),
	[THEME_INACTIVE_TITLE_TEXT]  = RGB(0xFF, 0xFF, 0xFF),
	[THEME_DESKTOP]              = RGB(0x00, 0x00, 0x00),
	[THEME_MENU_BG]              = RGB(0x00, 0x00, 0x00),
	[THEME_MENU_TEXT]            = RGB(0xFF, 0xFF, 0xFF),
	[THEME_HIGHLIGHT]            = RGB(0xFF, 0xFF, 0x00), /* 未検証: 黄色 */
	[THEME_HIGHLIGHT_TEXT]       = RGB(0x00, 0x00, 0x00),
	[THEME_WINDOW_BG]            = RGB(0x00, 0x00, 0x00),
	[THEME_WINDOW_TEXT]          = RGB(0xFF, 0xFF, 0xFF),
	[THEME_DISABLED_TEXT]        = RGB(0x80, 0x80, 0x80),
};

/* THEME_PRESET_* (draw.h) の並びと 1:1 対応させる */
static const uint32_t *const PRESET_TABLES[THEME_PRESET_COUNT] = {
	[THEME_PRESET_STANDARD]   = THEME_STANDARD,
	[THEME_PRESET_RAINY]      = THEME_RAINY,
	[THEME_PRESET_EGGPLANT]   = THEME_EGGPLANT,
	[THEME_PRESET_PLUM]       = THEME_PLUM,
	[THEME_PRESET_HICONTRAST] = THEME_HICONTRAST,
};

/*
 * 設定キー名 (color.<name> の <name> 部分、例えば "face" や
 * "active_title_l") から THEME_* 索引を引く表。
 */
struct color_key { const char *name; int index; };

static const struct color_key COLOR_KEYS[] = {
	{ "face",                  THEME_FACE },
	{ "hilight",                THEME_HILIGHT },
	{ "light",                  THEME_LIGHT },
	{ "shadow",                 THEME_SHADOW },
	{ "dkshadow",               THEME_DKSHADOW },
	{ "active_title_l",         THEME_ACTIVE_TITLE_L },
	{ "active_title_r",         THEME_ACTIVE_TITLE_R },
	{ "inactive_title_l",       THEME_INACTIVE_TITLE_L },
	{ "inactive_title_r",       THEME_INACTIVE_TITLE_R },
	{ "title_text",             THEME_TITLE_TEXT },
	{ "inactive_title_text",    THEME_INACTIVE_TITLE_TEXT },
	{ "desktop",                THEME_DESKTOP },
	{ "menu_bg",                THEME_MENU_BG },
	{ "menu_text",              THEME_MENU_TEXT },
	{ "highlight",              THEME_HIGHLIGHT },
	{ "highlight_text",         THEME_HIGHLIGHT_TEXT },
	{ "window_bg",              THEME_WINDOW_BG },
	{ "window_text",            THEME_WINDOW_TEXT },
	{ "disabled_text",          THEME_DISABLED_TEXT },
};
#define N_COLOR_KEYS (sizeof(COLOR_KEYS) / sizeof(COLOR_KEYS[0]))

int theme_color_index(const char *name)
{
	if (!name)
		return -1;
	for (size_t i = 0; i < N_COLOR_KEYS; i++) {
		if (strcmp(name, COLOR_KEYS[i].name) == 0)
			return COLOR_KEYS[i].index;
	}
	return -1;
}

static const struct { const char *name; int index; } PRESET_KEYS[] = {
	{ "standard",   THEME_PRESET_STANDARD },
	{ "rainy",      THEME_PRESET_RAINY },
	{ "eggplant",   THEME_PRESET_EGGPLANT },
	{ "plum",       THEME_PRESET_PLUM },
	{ "hicontrast", THEME_PRESET_HICONTRAST },
};
#define N_PRESET_KEYS (sizeof(PRESET_KEYS) / sizeof(PRESET_KEYS[0]))

int theme_preset_index(const char *name)
{
	if (!name)
		return -1;
	for (size_t i = 0; i < N_PRESET_KEYS; i++) {
		if (strcmp(name, PRESET_KEYS[i].name) == 0)
			return PRESET_KEYS[i].index;
	}
	/* "custom" は config.c 側の特別扱い（プリセットを適用しない）であり、
	 * THEME_PRESET_* に対応物が無いのでここでも -1 を返す。 */
	return -1;
}

void theme_load_preset(uint32_t *colors, int preset)
{
	if (preset < 0 || preset >= THEME_PRESET_COUNT)
		preset = THEME_PRESET_STANDARD; /* 防御的フォールバック */
	const uint32_t *src = PRESET_TABLES[preset];
	for (int i = 0; i < THEME_COLOR_COUNT; i++)
		colors[i] = src[i];
}

/* ================================================================== *
 * メトリクス (SPEC §4.2)
 * ================================================================== */

/* 96dpi 基準の生の値 (scale=1 相当)。ここ以外に生の定数を書かないこと。 */
static const struct metrics METRICS_BASE = {
	.border_sizing   = 4,
	.border_fixed    = 3,
	.caption_h       = 18,
	.caption_h_small = 13,
	.btn_w           = 16,
	.btn_h           = 14,
	.icon_size       = 16,
	.taskbar_h       = 28,
	.menu_item_h     = 18,
	.menu_sep_h      = 7,
	.scale           = 1,
};

static struct metrics g_metrics;
static bool g_metrics_ready = false;

const struct metrics *theme_metrics(void)
{
	if (!g_metrics_ready) {
		/* config 読み込み前に呼ばれた場合の防御。scale=1 相当を返す。 */
		theme_init();
	}
	return &g_metrics;
}

void theme_init(void)
{
	uint8_t scale = wm.cfg.scale;
	if (scale != 1 && scale != 2)
		scale = 1; /* config_load が 1/2 に検証済みのはずだが念のため */

	g_metrics.border_sizing   = (uint16_t)(METRICS_BASE.border_sizing   * scale);
	g_metrics.border_fixed    = (uint16_t)(METRICS_BASE.border_fixed    * scale);
	g_metrics.caption_h       = (uint16_t)(METRICS_BASE.caption_h       * scale);
	g_metrics.caption_h_small = (uint16_t)(METRICS_BASE.caption_h_small * scale);
	g_metrics.btn_w           = (uint16_t)(METRICS_BASE.btn_w           * scale);
	g_metrics.btn_h           = (uint16_t)(METRICS_BASE.btn_h           * scale);
	g_metrics.icon_size       = (uint16_t)(METRICS_BASE.icon_size       * scale);
	g_metrics.taskbar_h       = (uint16_t)(METRICS_BASE.taskbar_h       * scale);
	g_metrics.menu_item_h     = (uint16_t)(METRICS_BASE.menu_item_h     * scale);
	g_metrics.menu_sep_h      = (uint16_t)(METRICS_BASE.menu_sep_h      * scale);
	g_metrics.scale           = scale;

	g_metrics_ready = true;
}
