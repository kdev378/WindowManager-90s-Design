/*
 * shape.c - 非矩形ウィンドウのフレーム形状追従 (SPEC §5.3、任意)
 *
 * Shape 拡張は「任意」である。無い X サーバでは全ウィンドウを矩形として
 * 扱い、動作を継続する（起動失敗にはしない。SPEC §5.3 の表そのまま）。
 * これを 2 段構えで保証する:
 *
 *   1. コンパイル時: <xcb/shape.h> が無い環境でもビルドが通るよう、
 *      __has_include で存在を確認してから include する。この環境の
 *      Makefile は xcb-shape を REQ_PKGS に含めており通常はヘッダも
 *      ライブラリも揃っているが、「無くても壊れない」ことを自己完結して
 *      保証するのがこのファイルの役目である。
 *      検証用に WM_NO_SHAPE_HEADER を定義すると、ヘッダが実在していても
 *      強制的にフォールバック経路（3 関数とも no-op）を選ばせられる。
 *
 *   2. 実行時: xcb_get_extension_data() で拡張の有無を問い合わせ、
 *      wm.have_shape / wm.shape_base に記録する。無ければ shape_apply /
 *      shape_handle_event は何もしない。
 *
 * 対応: SPEC §5.3
 */
#include <stdlib.h>

#include "w98wm.h"

#ifndef WM_NO_SHAPE_HEADER   /* テスト専用フック。通常ビルドでは定義しない */
#if defined(__has_include)
#  if __has_include(<xcb/shape.h>)
#    define WM_SHAPE_HEADER_PRESENT 1
#  endif
#endif
#endif

#ifdef WM_SHAPE_HEADER_PRESENT
#include <xcb/shape.h>
#endif

#ifdef WM_SHAPE_HEADER_PRESENT

/* ================================================================== *
 * ヘッダ・ライブラリともに利用可能な場合
 * ================================================================== */

void shape_init(void)
{
	const xcb_query_extension_reply_t *ext;

	wm.have_shape = false;
	wm.shape_base = 0;

	ext = xcb_get_extension_data(wm.conn, &xcb_shape_id);
	if (ext != NULL && ext->present) {
		wm.have_shape = true;
		wm.shape_base = ext->first_event;
		LOG("shape: 拡張あり (first_event=%u)。非矩形ウィンドウに追従する (SPEC §5.3)",
		    (unsigned)wm.shape_base);
	} else {
		LOG("shape: 拡張なし。全ウィンドウを矩形として扱う (SPEC §5.3)");
	}
}

/*
 * クライアントの Bounding 形状をフレームへ複製する (SPEC §5.3)。
 *
 * SO_SET を使うため、フレームの Bounding 領域はクライアントの形状で
 * **完全に置き換わる**（装飾矩形との Union はしない）。非矩形になる
 * クライアント（xeyes, oclock, スプラッシュ等）は通常そもそも無装飾
 * なので、これで意図通り「矩形フレームが透けて見える」事故を防げる。
 * 装飾付きクライアントが特殊な形状を主張してきた場合は、装飾ごと
 * クライアントの形に切り取られる（矩形でない装飾は元々想定していない）。
 *
 * オフセットは client_frame_offsets() が返す「装飾原点」——
 * クライアント領域がフレーム内部でどれだけ内側にあるか——を使う。
 * クライアントに独自形状が無ければ (bounding_shaped == 0) フレームの
 * 形状をクリアし、通常の矩形へ戻す。
 */
void shape_apply(struct client *c)
{
	uint16_t left, right, top, bottom;
	xcb_shape_query_extents_cookie_t ck;
	xcb_shape_query_extents_reply_t *r;

	if (!wm.have_shape)
		return;
	if (c == NULL || c->win == XCB_WINDOW_NONE || c->frame == XCB_WINDOW_NONE)
		return;

	/*
	 * 今後クライアントが形状を変えたときに ShapeNotify を受け取れるよう
	 * 選択しておく。何度呼んでも安全（内部的にはマスクの置き換えのみ）。
	 */
	xcb_shape_select_input(wm.conn, c->win, 1);

	/*
	 * §2.2.2: クライアント宛のリクエストは常にエラーを許容する。
	 * ここは reply を待つ経路なので、消えかけのウィンドウに対しては
	 * reply が NULL で返るだけで済む（エラーイベント自体は event.c の
	 * 通常ディスパッチへ届き、そちらの方針で処理される）。
	 */
	ck = xcb_shape_query_extents(wm.conn, c->win);
	r  = xcb_shape_query_extents_reply(wm.conn, ck, NULL);
	if (r == NULL)
		return;

	client_frame_offsets(c, &left, &right, &top, &bottom);
	(void)right;
	(void)bottom;

	if (r->bounding_shaped) {
		xcb_shape_combine(wm.conn, XCB_SHAPE_SO_SET,
		                  XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
		                  c->frame, (int16_t)left, (int16_t)top, c->win);
	} else {
		/* 矩形に戻った（または最初から矩形）。フレームの形状をクリアする。
		 * source_bitmap に None を渡すと、その種別の形状は既定の矩形へ戻る。 */
		xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
		               c->frame, 0, 0, XCB_PIXMAP_NONE);
	}
	free(r);
}

/*
 * ShapeNotify への応答 (SPEC §5.3)。処理したら true。
 * Bounding 以外（Clip/Input）の変化は装飾の見た目に関係しないため無視する。
 */
bool shape_handle_event(xcb_generic_event_t *ev)
{
	const xcb_shape_notify_event_t *sn;
	struct client *c;

	if (!wm.have_shape || ev == NULL)
		return false;
	if ((ev->response_type & 0x7Fu) != (uint8_t)(wm.shape_base + XCB_SHAPE_NOTIFY))
		return false;

	sn = (const xcb_shape_notify_event_t *)ev;
	if (sn->shape_kind != XCB_SHAPE_SK_BOUNDING)
		return true;   /* 消費はするが再適用は不要 */

	c = client_find(sn->affected_window);
	if (c != NULL)
		shape_apply(c);
	return true;
}

#else /* !WM_SHAPE_HEADER_PRESENT */

/* ================================================================== *
 * ヘッダが無いビルド: Shape 機能ごと縮退し、全ウィンドウを矩形として
 * 扱う (SPEC §5.3)。3 関数とも no-op。
 * ================================================================== */

void shape_init(void)
{
	wm.have_shape = false;
	wm.shape_base = 0;
}

void shape_apply(struct client *c)
{
	(void)c;
}

bool shape_handle_event(xcb_generic_event_t *ev)
{
	(void)ev;
	return false;
}

#endif /* WM_SHAPE_HEADER_PRESENT */
