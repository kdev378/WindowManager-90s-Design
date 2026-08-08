/*
 * icccm.c - ICCCM / EWMH プロパティの読み取り
 *
 * 対応する仕様: docs/SPEC.md §5.1（実装する ICCCM の節）, §2.3.1（タイトル）,
 *               §5.2.2（種別）, §7.2（CSD）, §3.6（フォーカス）, §3.7.2（transient）
 *
 * 方針:
 *   - xcb-util-wm には依存せず、生のプロパティを自前で解釈する。
 *   - **クライアントは信用しない**。プロパティは「存在しない・型が違う・
 *     長さが足りない」の 3 通りで必ず壊れ得る。要素数を毎回検査し、
 *     宣言された長さを 1 要素でも越えて読まない。
 *   - 取得ヘルパ（prop_get_*）が返した配列は必ず free する。
 */
#include <stdlib.h>
#include <string.h>

#include "w98wm.h"

/* ------------------------------------------------------------------ *
 * ICCCM のフラグ定数（Xlib のヘッダを引かずに済ませるため自前で定義）
 * ------------------------------------------------------------------ */

/* WM_SIZE_HINTS.flags (ICCCM §4.1.2.3) */
enum {
	SZ_US_POSITION = 1u << 0,
	SZ_US_SIZE     = 1u << 1,
	SZ_P_POSITION  = 1u << 2,
	SZ_P_SIZE      = 1u << 3,
	SZ_P_MIN_SIZE  = 1u << 4,
	SZ_P_MAX_SIZE  = 1u << 5,
	SZ_P_RESIZE_INC= 1u << 6,
	SZ_P_ASPECT    = 1u << 7,
	SZ_P_BASE_SIZE = 1u << 8,
	SZ_P_WIN_GRAVITY = 1u << 9
};

/* WM_SIZE_HINTS の要素数。旧クライアントは base/gravity 抜きの 15 個を送る */
#define SZ_LEN_FULL   18
#define SZ_LEN_LEGACY 15

/* WM_HINTS.flags (ICCCM §4.1.2.4) */
enum {
	WH_INPUT         = 1u << 0,
	WH_STATE         = 1u << 1,
	WH_ICON_PIXMAP   = 1u << 2,
	WH_ICON_WINDOW   = 1u << 3,
	WH_ICON_POSITION = 1u << 4,
	WH_ICON_MASK     = 1u << 5,
	WH_WINDOW_GROUP  = 1u << 6,
	WH_MESSAGE       = 1u << 7,
	WH_URGENCY       = 1u << 8
};

#define WH_LEN 9

/* WM_STATE の状態値 (ICCCM §4.1.3.1) */
enum { STATE_WITHDRAWN = 0, STATE_NORMAL = 1, STATE_ICONIC = 3 };

/* ------------------------------------------------------------------ *
 * 小さなユーティリティ
 * ------------------------------------------------------------------ */

/*
 * ICCCM のサイズ系フィールドは INT32 である。負値や 16bit を越える値を
 * そのまま uint16_t へ落とすと巻き戻って「巨大な最小サイズ」等になるため、
 * 符号付きで受けてから飽和させる。
 */
static uint16_t sat_u16(uint32_t raw)
{
	int32_t v = (int32_t)raw;

	if (v <= 0)
		return 0;
	if (v > 0xFFFF)
		return 0xFFFF;
	return (uint16_t)v;
}

static int16_t sat_i16(int32_t v)
{
	if (v < -32768)
		return -32768;
	if (v > 32767)
		return 32767;
	return (int16_t)v;
}

/* ------------------------------------------------------------------ *
 * WM_NORMAL_HINTS (ICCCM §4.1.2.3)
 * ------------------------------------------------------------------ */

void icccm_update_size_hints(struct client *c)
{
	struct size_hints h;
	uint32_t *p;
	uint32_t n = 0, f;

	if (c == NULL)
		return;

	memset(&h, 0, sizeof(h));
	/* PWinGravity が無い場合の既定は NorthWest (ICCCM §4.1.2.3) */
	h.gravity = XCB_GRAVITY_NORTH_WEST;

	p = prop_get_card32_list(c->win, XCB_ATOM_WM_NORMAL_HINTS,
	                         XCB_ATOM_WM_SIZE_HINTS, &n);
	if (p == NULL || n < 1) {
		/* 無い・壊れている場合はヒント無しとして扱う（既定値のまま） */
		free(p);
		c->hints = h;
		c->flags &= ~(uint32_t)CF_FIXED_SIZE;
		return;
	}
	if (n > SZ_LEN_FULL)
		n = SZ_LEN_FULL;   /* 余分な後続要素は読まない */

	f = p[0];

	/* 位置・サイズの「指定された」フラグはそのまま写す（§3.3.1 の初期配置で使う） */
	if (f & SZ_US_POSITION)
		h.flags |= HINT_US_POSITION;
	if (f & SZ_US_SIZE)
		h.flags |= HINT_US_SIZE;
	if (f & SZ_P_POSITION)
		h.flags |= HINT_P_POSITION;
	if (f & SZ_P_SIZE)
		h.flags |= HINT_P_SIZE;

	/*
	 * ここから先はすべて「フラグが立っていること」に加えて
	 * 「その要素まで実際にプロパティが伸びていること」を要求する。
	 * 1 バイトだけの WM_NORMAL_HINTS を投げてくるクライアントがあり得る。
	 */
	if ((f & SZ_P_MIN_SIZE) && n >= 7) {
		h.min_w = sat_u16(p[5]);
		h.min_h = sat_u16(p[6]);
		h.flags |= HINT_MIN_SIZE;
	}
	if ((f & SZ_P_MAX_SIZE) && n >= 9) {
		h.max_w = sat_u16(p[7]);
		h.max_h = sat_u16(p[8]);
		h.flags |= HINT_MAX_SIZE;
	}
	if ((f & SZ_P_RESIZE_INC) && n >= 11) {
		h.inc_w = sat_u16(p[9]);
		h.inc_h = sat_u16(p[10]);
		/*
		 * 増分 0 は不正。hints_apply() 側の除算を守るため、
		 * 片方だけ 0 なら 1 に、両方 0 ならフラグごと落とす。
		 */
		if (h.inc_w == 0 && h.inc_h == 0) {
			h.inc_w = h.inc_h = 0;
		} else {
			if (h.inc_w == 0)
				h.inc_w = 1;
			if (h.inc_h == 0)
				h.inc_h = 1;
			h.flags |= HINT_RESIZE_INC;
		}
	}
	if ((f & SZ_P_ASPECT) && n >= 15) {
		h.min_aspect_num = (int32_t)p[11];
		h.min_aspect_den = (int32_t)p[12];
		h.max_aspect_num = (int32_t)p[13];
		h.max_aspect_den = (int32_t)p[14];
		/* 分母 0（ゼロ除算）と負のアスペクトは採用しない */
		if (h.min_aspect_den > 0 && h.max_aspect_den > 0 &&
		    h.min_aspect_num >= 0 && h.max_aspect_num >= 0)
			h.flags |= HINT_ASPECT;
		else
			h.min_aspect_num = h.min_aspect_den =
			    h.max_aspect_num = h.max_aspect_den = 0;
	}
	/* base/gravity は 15 要素しか送らない旧クライアントでは欠落する */
	if ((f & SZ_P_BASE_SIZE) && n >= SZ_LEN_FULL - 1) {
		h.base_w = sat_u16(p[15]);
		h.base_h = sat_u16(p[16]);
		h.flags |= HINT_BASE_SIZE;
	}
	if ((f & SZ_P_WIN_GRAVITY) && n >= SZ_LEN_FULL) {
		uint32_t g = p[17];
		/* 未知の値は既定の NorthWest に倒す */
		if (g >= XCB_GRAVITY_NORTH_WEST && g <= XCB_GRAVITY_STATIC) {
			h.gravity = (uint8_t)g;
			h.flags |= HINT_GRAVITY;
		}
	}

	/*
	 * ICCCM §4.1.2.3: base が無ければ min を、min が無ければ base を使う。
	 * 片方から補ったときは値が有効になるので対応するビットも立てる
	 * （hints_apply() は HINT_* を見て丸めるため、ビットを立てないと
	 *  補った値が使われない）。
	 */
	if ((h.flags & HINT_BASE_SIZE) && !(h.flags & HINT_MIN_SIZE)) {
		h.min_w = h.base_w;
		h.min_h = h.base_h;
		h.flags |= HINT_MIN_SIZE;
	} else if ((h.flags & HINT_MIN_SIZE) && !(h.flags & HINT_BASE_SIZE)) {
		h.base_w = h.min_w;
		h.base_h = h.min_h;
		h.flags |= HINT_BASE_SIZE;
	}

	/* max < min は仕様上未定義。min 側に寄せて矛盾を消す */
	if ((h.flags & HINT_MIN_SIZE) && (h.flags & HINT_MAX_SIZE)) {
		if (h.max_w < h.min_w)
			h.max_w = h.min_w;
		if (h.max_h < h.min_h)
			h.max_h = h.min_h;
	}

	free(p);
	c->hints = h;

	/* min == max（両軸・両方指定）ならリサイズ不可。§4.4 のボタン活性に使う */
	c->flags &= ~(uint32_t)CF_FIXED_SIZE;
	if ((h.flags & HINT_MIN_SIZE) && (h.flags & HINT_MAX_SIZE) &&
	    h.min_w > 0 && h.min_h > 0 &&
	    h.min_w == h.max_w && h.min_h == h.max_h)
		c->flags |= CF_FIXED_SIZE;
}

/* ------------------------------------------------------------------ *
 * WM_HINTS (ICCCM §4.1.2.4)
 * ------------------------------------------------------------------ */

void icccm_update_wm_hints(struct client *c)
{
	uint32_t *p;
	uint32_t n = 0, f;
	uint32_t initial_state = STATE_NORMAL;

	if (c == NULL)
		return;

	p = prop_get_card32_list(c->win, XCB_ATOM_WM_HINTS,
	                         XCB_ATOM_WM_HINTS, &n);
	if (p == NULL || n < 1) {
		free(p);
		/*
		 * WM_HINTS 自体が無い場合、input は「真とみなす」のが ICCCM の既定。
		 * ここを false にすると Passive 入力モデルのクライアントに
		 * フォーカスが渡らなくなる (§3.6)。
		 */
		c->flags |= CF_INPUT_HINT;
		c->flags &= ~(uint32_t)CF_URGENT;
		c->group = XCB_NONE;
		return;
	}
	if (n > WH_LEN)
		n = WH_LEN;

	f = p[0];

	/*
	 * InputHint が立っていないときは input フィールドが未定義なので読まない。
	 * ICCCM 上「指定が無ければ真とみなしてよい」（No Input モデルを名乗る
	 * クライアントは必ず InputHint を立てて input=False を送る）。
	 */
	if ((f & WH_INPUT) && n >= 2) {
		if (p[1] != 0)
			c->flags |= CF_INPUT_HINT;
		else
			c->flags &= ~(uint32_t)CF_INPUT_HINT;
	} else {
		c->flags |= CF_INPUT_HINT;
	}

	if ((f & WH_STATE) && n >= 3) {
		if (p[2] == STATE_ICONIC || p[2] == STATE_NORMAL ||
		    p[2] == STATE_WITHDRAWN)
			initial_state = p[2];
	}

	/*
	 * icon_pixmap(3) / icon_window(4) / icon_x,y(5,6) / icon_mask(7) は
	 * ここでは意図的に無視する。c->icon_pix / c->icon_mask は §4.4.1 に従って
	 * WM 側が作った 16x16 の Pixmap を持つフィールドであり、クライアント所有の
	 * Drawable ID をそこへ入れると解放時に他プロセスのリソースを FreePixmap
	 * してしまう。WM_HINTS のアイコンを使う処理は icon 側モジュールの担当。
	 */

	if ((f & WH_WINDOW_GROUP) && n >= 9)
		c->group = p[8];
	else
		c->group = XCB_NONE;

	/* XUrgencyHint。タスクバーボタンの点滅 (§3.7.2, §4.8) に使う */
	if (f & WH_URGENCY)
		c->flags |= CF_URGENT;
	else
		c->flags &= ~(uint32_t)CF_URGENT;

	free(p);

	/*
	 * initial_state の受け渡し:
	 *   struct client に initial_state を保持する場所が無い（ヘッダの制約）。
	 *   ICCCM §4.1.4 の「IconicState で起動したい」要求を落とさないよう、
	 *   まだ map も adopt もしていないクライアントに限り CF_ICONIC を立てて
	 *   client.c へ伝える。既に管理下にあるウィンドウの WM_HINTS 更新では
	 *   状態を書き換えない（実状態は WM_STATE と ST_HIDDEN が持つ）。
	 */
	if (initial_state == STATE_ICONIC &&
	    !(c->flags & (CF_MAPPED | CF_ADOPTED)))
		c->flags |= CF_ICONIC;
}

/* ------------------------------------------------------------------ *
 * WM_PROTOCOLS (ICCCM §4.1.2.7)
 * ------------------------------------------------------------------ */

void icccm_update_protocols(struct client *c)
{
	uint32_t *p;
	uint32_t n = 0, i;

	if (c == NULL)
		return;

	c->flags &= ~(uint32_t)(CF_DELETE_WINDOW | CF_TAKE_FOCUS | CF_PING);

	p = prop_get_card32_list(c->win, atoms[ATOM_WM_PROTOCOLS],
	                         XCB_ATOM_ATOM, &n);
	if (p == NULL)
		return;

	for (i = 0; i < n; i++) {
		xcb_atom_t a = (xcb_atom_t)p[i];

		if (a == XCB_ATOM_NONE)
			continue;
		if (a == atoms[ATOM_WM_DELETE_WINDOW])
			c->flags |= CF_DELETE_WINDOW;
		else if (a == atoms[ATOM_WM_TAKE_FOCUS])
			c->flags |= CF_TAKE_FOCUS;
		else if (a == atoms[ATOM_NET_WM_PING])
			c->flags |= CF_PING;
	}
	free(p);
}

/* ------------------------------------------------------------------ *
 * WM_TRANSIENT_FOR (ICCCM §4.1.2.6, SPEC §3.7.2)
 * ------------------------------------------------------------------ */

void icccm_update_transient(struct client *c)
{
	uint32_t v = 0;

	if (c == NULL)
		return;

	if (!prop_get_card32(c->win, XCB_ATOM_WM_TRANSIENT_FOR,
	                     XCB_ATOM_WINDOW, &v)) {
		c->transient_for = XCB_NONE;
		return;
	}

	/*
	 * 自分自身（および自分のフレーム）を指す値は循環そのものなので捨てる。
	 * stack.c 側にも 16 段の打ち切りがあるが、1 段目で潰せるものは潰す。
	 */
	if (v == c->win || v == c->frame || v == XCB_NONE) {
		c->transient_for = XCB_NONE;
		return;
	}

	/*
	 * ルートウィンドウを指す場合は「ウィンドウグループ全体に対する transient」
	 * を意味する (ICCCM §4.1.2.6, SPEC §3.7.2)。ここで NONE に潰すと
	 * その情報が失われるため、値をそのまま保持して stack.c に解釈させる。
	 */
	c->transient_for = (xcb_window_t)v;
}

/* ------------------------------------------------------------------ *
 * タイトル (_NET_WM_NAME / WM_NAME, SPEC §2.3.1)
 * ------------------------------------------------------------------ */

/* 除去対象のコードポイント (SPEC §2.3.1) */
static bool cp_is_forbidden(uint32_t cp)
{
	if (cp <= 0x1F || cp == 0x7F)          /* C0 と DEL */
		return true;
	if (cp >= 0x80 && cp <= 0x9F)          /* C1 */
		return true;
	if (cp >= 0x202A && cp <= 0x202E)      /* LRE/RLE/PDF/LRO/RLO */
		return true;
	if (cp >= 0x2066 && cp <= 0x2069)      /* LRI/RLI/FSI/PDI */
		return true;
	return false;
}

/*
 * UTF-8 を走査して、不正シーケンスと禁止コードポイントを取り除きつつ dst へ写す。
 * SPEC §2.3.1 に従い、サニタイズはプロパティ受信時のこの 1 回だけ行う。
 * 不正バイトは（U+FFFD へ広げず）捨てる。バッファを増やさずに済み、
 * 残った正当なシーケンスの描画には影響しない。必ず 1 バイト以上進むので
 * 無限ループにはならない。
 */
static size_t title_sanitize(const char *src, size_t len,
                             char *dst, size_t dmax)
{
	size_t i = 0, out = 0;

	while (i < len && out < dmax) {
		unsigned char b = (unsigned char)src[i];
		size_t need, k;
		uint32_t cp;

		if (b < 0x80) {
			need = 1;
			cp = b;
		} else if ((b & 0xE0) == 0xC0) {
			need = 2;
			cp = b & 0x1Fu;
		} else if ((b & 0xF0) == 0xE0) {
			need = 3;
			cp = b & 0x0Fu;
		} else if ((b & 0xF8) == 0xF0) {
			need = 4;
			cp = b & 0x07u;
		} else {
			i++;      /* 継続バイト単独 / 0xF8 以上 */
			continue;
		}

		if (i + need > len)
			break;    /* 途中で切れた末尾は捨てる */

		for (k = 1; k < need; k++) {
			unsigned char cb = (unsigned char)src[i + k];
			if ((cb & 0xC0) != 0x80)
				break;
			cp = (cp << 6) | (cb & 0x3Fu);
		}
		if (k != need) {
			i++;      /* 継続バイトが足りない: 1 バイト進めてやり直す */
			continue;
		}

		/* 冗長符号化・サロゲート・範囲外を弾く */
		if ((need == 2 && cp < 0x80) ||
		    (need == 3 && cp < 0x800) ||
		    (need == 4 && cp < 0x10000) ||
		    cp > 0x10FFFF ||
		    (cp >= 0xD800 && cp <= 0xDFFF)) {
			i += need;
			continue;
		}
		if (cp_is_forbidden(cp)) {
			i += need;
			continue;
		}
		if (out + need > dmax)
			break;    /* 途中のコードポイントで切らない */

		memcpy(dst + out, src + i, need);
		out += need;
		i += need;
	}
	return out;
}

void icccm_update_title(struct client *c)
{
	char buf[WM_TITLE_MAX];
	size_t len, cut, out;

	if (c == NULL)
		return;

	/* _NET_WM_NAME (UTF8_STRING) を優先し、無ければ WM_NAME に落とす */
	len = prop_get_text(c->win, atoms[ATOM_NET_WM_NAME], buf, sizeof(buf));
	if (len == 0)
		len = prop_get_text(c->win, XCB_ATOM_WM_NAME, buf, sizeof(buf));

	if (len > sizeof(buf))
		len = sizeof(buf);   /* ヘルパの戻り値も信用しない */

	/* UTF-8 のコードポイント境界で切る。NUL の分を 1 残す (§2.3.1) */
	cut = utf8_truncate_len(buf, len, WM_TITLE_MAX - 1);
	if (cut > len)
		cut = len;

	out = title_sanitize(buf, cut, c->title, WM_TITLE_MAX - 1);
	c->title[out] = '\0';
	c->title_len = (uint8_t)out;

	/*
	 * 計測キャッシュの無効化 (SPEC §4.5.2.1)。
	 * Expose の経路では文字幅を計測してはならないので、
	 * 「タイトルが変わったらここで捨てる」ことでキャッシュの整合を保つ。
	 */
	c->draw_glyphs = 0;
	c->caption_w_at_measure = 0;
	c->draw_ellipsis = 0;
}

/* ------------------------------------------------------------------ *
 * _NET_WM_WINDOW_TYPE (SPEC §5.2.2)
 * ------------------------------------------------------------------ */

static const struct {
	int           atom_id;
	enum win_type type;
} type_table[] = {
	{ ATOM_NET_WM_WINDOW_TYPE_DESKTOP,       TYPE_DESKTOP },
	{ ATOM_NET_WM_WINDOW_TYPE_DOCK,          TYPE_DOCK },
	{ ATOM_NET_WM_WINDOW_TYPE_TOOLBAR,       TYPE_TOOLBAR },
	{ ATOM_NET_WM_WINDOW_TYPE_MENU,          TYPE_MENU },
	{ ATOM_NET_WM_WINDOW_TYPE_UTILITY,       TYPE_UTILITY },
	{ ATOM_NET_WM_WINDOW_TYPE_SPLASH,        TYPE_SPLASH },
	{ ATOM_NET_WM_WINDOW_TYPE_DIALOG,        TYPE_DIALOG },
	{ ATOM_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, TYPE_DROPDOWN_MENU },
	{ ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU,    TYPE_POPUP_MENU },
	{ ATOM_NET_WM_WINDOW_TYPE_TOOLTIP,       TYPE_TOOLTIP },
	{ ATOM_NET_WM_WINDOW_TYPE_NOTIFICATION,  TYPE_NOTIFICATION },
	{ ATOM_NET_WM_WINDOW_TYPE_COMBO,         TYPE_COMBO },
	{ ATOM_NET_WM_WINDOW_TYPE_DND,           TYPE_DND },
	{ ATOM_NET_WM_WINDOW_TYPE_NORMAL,        TYPE_NORMAL }
};

void icccm_update_window_type(struct client *c)
{
	const struct type_props *tp;
	uint32_t *p;
	uint32_t n = 0, i;
	size_t j;
	bool found = false;

	if (c == NULL)
		return;

	c->type = TYPE_NORMAL;

	p = prop_get_card32_list(c->win, atoms[ATOM_NET_WM_WINDOW_TYPE],
	                         XCB_ATOM_ATOM, &n);
	if (p != NULL) {
		/*
		 * SPEC §5.2.2 決定規則 1: 先頭から走査し、**認識できた最初の
		 * ATOM** を採用する。認識できないものは読み飛ばす
		 * （独自 ATOM を先頭に置くアプリがあるため、
		 *  「先頭要素だけ見る」実装は誤り）。
		 */
		for (i = 0; i < n && !found; i++) {
			xcb_atom_t a = (xcb_atom_t)p[i];

			if (a == XCB_ATOM_NONE)
				continue;
			for (j = 0; j < sizeof(type_table) / sizeof(type_table[0]); j++) {
				if (a == atoms[type_table[j].atom_id]) {
					c->type = type_table[j].type;
					found = true;
					break;
				}
			}
		}
		free(p);
	}

	/*
	 * SPEC §5.2.2 決定規則 2: 認識できる ATOM が 1 つも無い、または
	 * プロパティが無い場合、WM_TRANSIENT_FOR があれば DIALOG とみなす。
	 * transient_for は先に icccm_update_transient() で読まれている前提だが、
	 * 順序に依存しないよう未取得なら 0 で NORMAL に落ちるだけで済ませる。
	 */
	if (!found)
		c->type = (c->transient_for != XCB_NONE) ? TYPE_DIALOG : TYPE_NORMAL;

	/*
	 * 種別ごとの基本レイヤを入れておく。ST_ABOVE/BELOW・全画面・transient を
	 * 含めた実効レイヤは stack_effective_layer() が再計算する (§3.7.1)。
	 */
	tp = type_props(c->type);
	if (tp != NULL)
		c->layer = tp->layer;
}

/* ------------------------------------------------------------------ *
 * _NET_WM_STATE (SPEC §5.2)
 * ------------------------------------------------------------------ */

static const struct {
	int      atom_id;
	uint32_t bit;
} state_table[] = {
	{ ATOM_NET_WM_STATE_MODAL,             ST_MODAL },
	{ ATOM_NET_WM_STATE_STICKY,            ST_STICKY },
	{ ATOM_NET_WM_STATE_MAXIMIZED_VERT,    ST_MAXIMIZED_VERT },
	{ ATOM_NET_WM_STATE_MAXIMIZED_HORZ,    ST_MAXIMIZED_HORZ },
	{ ATOM_NET_WM_STATE_SHADED,            ST_SHADED },
	{ ATOM_NET_WM_STATE_SKIP_TASKBAR,      ST_SKIP_TASKBAR },
	{ ATOM_NET_WM_STATE_SKIP_PAGER,        ST_SKIP_PAGER },
	{ ATOM_NET_WM_STATE_HIDDEN,            ST_HIDDEN },
	{ ATOM_NET_WM_STATE_FULLSCREEN,        ST_FULLSCREEN },
	{ ATOM_NET_WM_STATE_ABOVE,             ST_ABOVE },
	{ ATOM_NET_WM_STATE_BELOW,             ST_BELOW },
	{ ATOM_NET_WM_STATE_DEMANDS_ATTENTION, ST_DEMANDS_ATTENTION },
	{ ATOM_NET_WM_STATE_FOCUSED,           ST_FOCUSED }
};

/*
 * 既存ウィンドウの取り込み（adopt）時に、クライアントが自分で設定していた
 * _NET_WM_STATE を読む。ST_FOCUSED は WM が所有する状態なので
 * クライアントの申告では書き換えない。
 */
void icccm_update_states(struct client *c)
{
	uint32_t *p;
	uint32_t n = 0, i, st = 0;
	size_t j;

	if (c == NULL)
		return;

	p = prop_get_card32_list(c->win, atoms[ATOM_NET_WM_STATE],
	                         XCB_ATOM_ATOM, &n);
	if (p == NULL)
		return;

	for (i = 0; i < n; i++) {
		xcb_atom_t a = (xcb_atom_t)p[i];

		if (a == XCB_ATOM_NONE)
			continue;
		for (j = 0; j < sizeof(state_table) / sizeof(state_table[0]); j++) {
			if (a == atoms[state_table[j].atom_id]) {
				st |= state_table[j].bit;
				break;
			}
		}
	}
	free(p);

	c->states = (st & ~(uint32_t)ST_FOCUSED) | (c->states & ST_FOCUSED);
}

/* ------------------------------------------------------------------ *
 * _GTK_FRAME_EXTENTS (SPEC §7.2)
 * ------------------------------------------------------------------ */

void icccm_update_gtk_extents(struct client *c)
{
	uint32_t *p;
	uint32_t n = 0;

	if (c == NULL)
		return;

	p = prop_get_card32_list(c->win, atoms[ATOM_GTK_FRAME_EXTENTS],
	                         XCB_ATOM_CARDINAL, &n);
	if (p == NULL || n < 4) {
		/*
		 * プロパティが消えた（あるいは 4 要素に満たない壊れた値）なら
		 * CSD ではないものとして扱う。§7.2 のとおりこの関数はいつでも
		 * 読み直せる必要があり、状態遷移のたびに呼ばれる。
		 */
		free(p);
		c->flags &= ~(uint32_t)CF_CSD;
		c->gtk_extents[0] = c->gtk_extents[1] = 0;
		c->gtk_extents[2] = c->gtk_extents[3] = 0;
		return;
	}

	/* left, right, top, bottom。全画面時に GTK は 0 を書いてくる (§7.2) */
	c->gtk_extents[0] = sat_u16(p[0]);
	c->gtk_extents[1] = sat_u16(p[1]);
	c->gtk_extents[2] = sat_u16(p[2]);
	c->gtk_extents[3] = sat_u16(p[3]);
	free(p);

	/*
	 * CF_CSD は「値が非ゼロか」ではなく「プロパティが存在するか」で決める。
	 * 全画面の GTK ウィンドウは (0,0,0,0) を持つが CSD のままであり、
	 * ここで CSD を落とすと復帰時に装飾が二重になる。
	 */
	c->flags |= CF_CSD;
}

/* ------------------------------------------------------------------ *
 * ウィンドウグラビティ (ICCCM §4.1.5)
 * ------------------------------------------------------------------ */

/*
 * ConfigureRequest の位置要求にグラビティを適用する。
 *
 * 入出力の約束:
 *   入力 *x, *y … クライアントが要求した「自分のウィンドウの位置」
 *                 （X の座標系なのでボーダー矩形の外側左上。ボーダー幅は bw_new）
 *   出力 *x, *y … 装飾を付けた上でクライアント領域を置くべきルート座標
 *                 （= struct client.geom.x/y）
 *
 * 導出: 再親化した我々のフレーム矩形が、クライアントの「ボーダー矩形」の役割を
 * 引き継ぐ。よって「グラビティが指す基準点において、フレーム矩形と
 * 要求されたボーダー矩形が一致する」ように置けばよい。幅・高さは両辺で
 * 打ち消し合うため、ここでサイズを知る必要はない。
 *   例) NorthWest: フレーム左上 = 要求位置        → client_x = x + left
 *       East     : フレーム右端 = 要求右端        → client_x = x + 2*bw_new - right
 *       Center   : フレーム中心 = 要求中心        → client_x = x + bw_new + (left-right)/2
 *       Static   : クライアント原点そのものを固定 → client_x = x + bw_old
 *
 * bw_old/bw_new: ボーダー幅が変わると各列（左/中央/右）で保存すべき辺の位置が
 * ずれるので、d = bw_old - bw_new を列ごとの不変量に合わせて足し込む
 * （左端固定なら 0、中心固定なら d、右端固定なら 2d）。
 * 我々は reparent 時にクライアントのボーダーを 0 にするため、実運用では
 * ほとんどの場合 bw_old = bw_new = 0 となり、これらの項は消える。
 */
void icccm_apply_gravity(const struct client *c, uint8_t gravity,
                         uint16_t bw_old, uint16_t bw_new,
                         int16_t *x, int16_t *y)
{
	uint16_t left = 0, right = 0, top = 0, bottom = 0;
	int32_t l, r, t, b, d, bn;
	int32_t nx, ny;

	if (c == NULL || x == NULL || y == NULL)
		return;

	client_frame_offsets(c, &left, &right, &top, &bottom);
	l = (int32_t)left;
	r = (int32_t)right;
	t = (int32_t)top;
	b = (int32_t)bottom;
	d  = (int32_t)bw_old - (int32_t)bw_new;
	bn = (int32_t)bw_new;

	nx = *x;
	ny = *y;

	/* Forget(0) と未知の値は NorthWest として扱う (ICCCM §4.1.2.3) */
	if (gravity == XCB_GRAVITY_BIT_FORGET || gravity > XCB_GRAVITY_STATIC)
		gravity = XCB_GRAVITY_NORTH_WEST;

	if (gravity == XCB_GRAVITY_STATIC) {
		/*
		 * Static: クライアントは「フレームがどうであろうと、自分の
		 * ウィンドウは指定した座標のままにしてほしい」と言っている。
		 * 装飾の厚みを一切足さないのが正解（ここを NorthWest と同じに
		 * 書いてしまう実装が多い）。
		 */
		nx += d + bn;
		ny += d + bn;
		*x = sat_i16(nx);
		*y = sat_i16(ny);
		return;
	}

	/* --- 水平方向: 左列 / 中央列 / 右列 --- */
	switch (gravity) {
	case XCB_GRAVITY_NORTH_WEST:
	case XCB_GRAVITY_WEST:
	case XCB_GRAVITY_SOUTH_WEST:
		nx += l;                       /* 左端固定 */
		break;
	case XCB_GRAVITY_NORTH:
	case XCB_GRAVITY_CENTER:
	case XCB_GRAVITY_SOUTH:
		nx += d + bn + (l - r) / 2;    /* 中心固定 */
		break;
	default:                               /* NE / E / SE */
		nx += 2 * d + 2 * bn - r;      /* 右端固定 */
		break;
	}

	/* --- 垂直方向: 上行 / 中央行 / 下行 --- */
	switch (gravity) {
	case XCB_GRAVITY_NORTH_WEST:
	case XCB_GRAVITY_NORTH:
	case XCB_GRAVITY_NORTH_EAST:
		ny += t;                       /* 上端固定 */
		break;
	case XCB_GRAVITY_WEST:
	case XCB_GRAVITY_CENTER:
	case XCB_GRAVITY_EAST:
		ny += d + bn + (t - b) / 2;    /* 中心固定 */
		break;
	default:                               /* SW / S / SE */
		ny += 2 * d + 2 * bn - b;      /* 下端固定 */
		break;
	}

	*x = sat_i16(nx);
	*y = sat_i16(ny);
}
