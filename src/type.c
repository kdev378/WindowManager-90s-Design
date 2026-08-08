/*
 * type.c - ウィンドウ種別ごとの属性表 (SPEC §5.2.2)
 *
 * enum win_type（w98wm.h）の並び順表と 1:1 対応する定数テーブル。
 * 実行時に書き換えない。表の出典は SPEC §5.2.2 の表そのもの:
 *
 *   種別              装飾           レイヤ    フォーカス タスクバー
 *   DESKTOP           無             desktop   可         非表示
 *   DOCK              無             dock      不可       非表示
 *   TOOLBAR           小キャプション normal    可         非表示
 *   MENU              小キャプション normal    可         非表示
 *   UTILITY           小キャプション normal    可         非表示
 *   SPLASH            無             normal    不可       非表示
 *   DIALOG            有([X]のみ)    normal    可         表示
 *   DROPDOWN_MENU     無             above     不可       非表示
 *   POPUP_MENU        無             above     不可       非表示
 *   TOOLTIP           無             above     不可       非表示
 *   NOTIFICATION      無             above     不可       非表示
 *   COMBO             無             above     不可       非表示
 *   DND               無             above     不可       非表示
 *   NORMAL            有             normal    可         表示
 *
 * DIALOG は「[X] のみの装飾」という §4.4 の特殊描画が要るが、それはキャプション
 * ボタンの描画側（他モジュール）の仕事であり、ここでは small_caption=false
 * （13px 小キャプションではなく通常キャプション）として表す。
 */
#include "w98wm.h"

static const struct type_props type_table[TYPE_COUNT] = {
	[TYPE_DESKTOP] = {
		.decorated = false, .small_caption = false,
		.focusable = true,  .in_taskbar = false,
		.layer = LAYER_DESKTOP,
	},
	[TYPE_DOCK] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_DOCK,
	},
	[TYPE_TOOLBAR] = {
		.decorated = true,  .small_caption = true,
		.focusable = true,  .in_taskbar = false,
		.layer = LAYER_NORMAL,
	},
	[TYPE_MENU] = {
		.decorated = true,  .small_caption = true,
		.focusable = true,  .in_taskbar = false,
		.layer = LAYER_NORMAL,
	},
	[TYPE_UTILITY] = {
		.decorated = true,  .small_caption = true,
		.focusable = true,  .in_taskbar = false,
		.layer = LAYER_NORMAL,
	},
	[TYPE_SPLASH] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_NORMAL,
	},
	[TYPE_DIALOG] = {
		.decorated = true,  .small_caption = false,  /* [X] のみ (§4.4) */
		.focusable = true,  .in_taskbar = true,
		.layer = LAYER_NORMAL,
	},
	[TYPE_DROPDOWN_MENU] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_POPUP_MENU] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_TOOLTIP] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_NOTIFICATION] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_COMBO] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_DND] = {
		.decorated = false, .small_caption = false,
		.focusable = false, .in_taskbar = false,
		.layer = LAYER_ABOVE,
	},
	[TYPE_NORMAL] = {
		.decorated = true,  .small_caption = false,
		.focusable = true,  .in_taskbar = true,
		.layer = LAYER_NORMAL,
	},
};

const struct type_props *type_props(enum win_type t)
{
	if (t >= TYPE_COUNT)
		return &type_table[TYPE_NORMAL];   /* 未知の値は NORMAL 相当に縮退 */
	return &type_table[t];
}
