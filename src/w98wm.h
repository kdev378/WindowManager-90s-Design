/*
 * w98wm.h - 中核の型定義とモジュール間の契約
 *
 * 対応する仕様: docs/SPEC.md
 * このヘッダが全モジュールの唯一の接点。実装ファイルは相互に .c を include しない。
 */
#ifndef W98WM_H
#define W98WM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/sync.h>

#include "brand.h"
#include "compat.h"
#include "atoms.h"

/* ================================================================== *
 * 定数（SPEC §2.3.0, §3.7.1, §4.2）
 * ================================================================== */

#define WM_MAX_CLIENTS      512   /* これを超えたら装飾なしで野に放つ (§2.3.0) */
#define WM_SLAB_CHUNK        64   /* チャンク 0 は BSS に静的確保 (§2.3.0) */
#define WM_TITLE_MAX        256   /* タイトルキャッシュ (§2.3.1) */
#define WM_MAX_DESKTOPS      16   /* (§3.8) */
#define WM_MAX_MONITORS      16
#define WM_MAX_BINDINGS     128   /* (§8) */
#define WM_TRANSIENT_DEPTH   16   /* 循環検出の打ち切り (§3.7.2) */

#define WM_DRAG_INTERVAL_MS  16   /* 約 60Hz のレート上限 (§3.4.1) */
#define WM_SYNC_TIMEOUT_MS  250   /* WAITING → STALLED (§7.3) */
#define WM_PING_TIMEOUT_MS 5000   /* _NET_WM_PING (§7.4) */

#define WM_ALL_DESKTOPS  0xFFFFFFFFu

/* ================================================================== *
 * 幾何
 * ================================================================== */

struct rect {
	int16_t  x, y;
	uint16_t w, h;
};

static inline bool rect_valid(const struct rect *r) { return r->w > 0 && r->h > 0; }

/* 交差面積。モニタ選択のフォールバックに使う (§3.5) */
uint32_t rect_overlap_area(const struct rect *a, const struct rect *b);
bool     rect_contains_point(const struct rect *r, int16_t x, int16_t y);
void     rect_center(const struct rect *r, int16_t *cx, int16_t *cy);

/* ================================================================== *
 * WM_NORMAL_HINTS の要約 (ICCCM §4.1.2.3)
 * ================================================================== */

enum {
	HINT_US_POSITION = 1u << 0,
	HINT_US_SIZE     = 1u << 1,
	HINT_P_POSITION  = 1u << 2,
	HINT_P_SIZE      = 1u << 3,
	HINT_MIN_SIZE    = 1u << 4,
	HINT_MAX_SIZE    = 1u << 5,
	HINT_RESIZE_INC  = 1u << 6,
	HINT_ASPECT      = 1u << 7,
	HINT_BASE_SIZE   = 1u << 8,
	HINT_GRAVITY     = 1u << 9
};

struct size_hints {
	uint32_t flags;
	uint16_t min_w, min_h;
	uint16_t max_w, max_h;
	uint16_t inc_w, inc_h;
	uint16_t base_w, base_h;
	int32_t  min_aspect_num, min_aspect_den;
	int32_t  max_aspect_num, max_aspect_den;
	uint8_t  gravity;          /* XCB_GRAVITY_* */
};

/* 要求サイズをヒントに合わせて丸める。増分・アスペクト・最小最大を適用 (§3.4) */
void hints_apply(const struct size_hints *h, uint16_t *w, uint16_t *h_out);

/* ================================================================== *
 * ウィンドウ種別 (SPEC §5.2.2) — 表の順序と一致させること
 * ================================================================== */

enum win_type {
	TYPE_DESKTOP = 0,
	TYPE_DOCK,
	TYPE_TOOLBAR,
	TYPE_MENU,
	TYPE_UTILITY,
	TYPE_SPLASH,
	TYPE_DIALOG,
	TYPE_DROPDOWN_MENU,
	TYPE_POPUP_MENU,
	TYPE_TOOLTIP,
	TYPE_NOTIFICATION,
	TYPE_COMBO,
	TYPE_DND,
	TYPE_NORMAL,
	TYPE_COUNT
};

/* 種別ごとの属性表 (§5.2.2)。type.c が持つ。 */
struct type_props {
	bool decorated;     /* 装飾を付けるか */
	bool small_caption; /* 13px の小キャプションか */
	bool focusable;
	bool in_taskbar;
	uint8_t layer;      /* LAYER_* */
};
const struct type_props *type_props(enum win_type t);

/* ================================================================== *
 * スタッキングのレイヤ (SPEC §3.7.1)
 * ================================================================== */

enum {
	LAYER_DESKTOP = 0,
	LAYER_BELOW,
	LAYER_NORMAL,
	LAYER_DOCK,
	LAYER_ABOVE,
	LAYER_FULLSCREEN,
	LAYER_COUNT
};

/* ================================================================== *
 * _NET_WM_STATE のビット (SPEC §5.2)
 * ================================================================== */

enum {
	ST_MODAL             = 1u << 0,
	ST_STICKY            = 1u << 1,
	ST_MAXIMIZED_VERT    = 1u << 2,
	ST_MAXIMIZED_HORZ    = 1u << 3,
	ST_SHADED            = 1u << 4,
	ST_SKIP_TASKBAR      = 1u << 5,
	ST_SKIP_PAGER        = 1u << 6,
	ST_HIDDEN            = 1u << 7,
	ST_FULLSCREEN        = 1u << 8,
	ST_ABOVE             = 1u << 9,
	ST_BELOW             = 1u << 10,
	ST_DEMANDS_ATTENTION = 1u << 11,
	ST_FOCUSED           = 1u << 12
};
#define ST_MAXIMIZED (ST_MAXIMIZED_VERT | ST_MAXIMIZED_HORZ)

/* ================================================================== *
 * クライアントのフラグ
 * ================================================================== */

enum {
	CF_MAPPED        = 1u << 0,  /* frame が map されている */
	CF_ICONIC        = 1u << 1,  /* 最小化中 (WM_STATE=Iconic) */
	CF_DECORATED     = 1u << 2,  /* 装飾を描くか */
	CF_INPUT_HINT    = 1u << 3,  /* WM_HINTS.input */
	CF_TAKE_FOCUS    = 1u << 4,  /* WM_TAKE_FOCUS を持つ */
	CF_DELETE_WINDOW = 1u << 5,  /* WM_DELETE_WINDOW を持つ */
	CF_PING          = 1u << 6,  /* _NET_WM_PING を持つ */
	CF_URGENT        = 1u << 7,  /* WM_HINTS.urgency */
	CF_CSD           = 1u << 8,  /* _GTK_FRAME_EXTENTS を持つ (§7.2) */
	CF_SYNC_UNFIT    = 1u << 9,  /* 同期不適合として記録済み (§7.3) */
	CF_DESTROYED     = 1u << 10, /* DestroyNotify 受領済み。二重 unmanage 防止 */
	CF_ADOPTED       = 1u << 11, /* 起動時に既存ウィンドウとして取り込んだ */
	CF_FIXED_SIZE    = 1u << 12  /* min==max でリサイズ不可 */
};

/* _NET_WM_SYNC_REQUEST の状態 (SPEC §7.3) */
enum sync_state { SYNC_IDLE = 0, SYNC_WAITING, SYNC_STALLED, SYNC_NONE };

/* ================================================================== *
 * クライアント (SPEC §2.3)
 * ================================================================== */

struct client {
	xcb_window_t frame;
	xcb_window_t win;

	struct rect  geom;        /* クライアント領域（frame 内部ではなくルート座標） */
	struct rect  restore;     /* 非最大化・非全画面のジオメトリ (§3.5.1) */
	struct rect  pre_fs;      /* 全画面直前のジオメトリ (§3.5.1) */
	uint32_t     pre_fs_states;

	struct size_hints hints;
	uint32_t     states;      /* ST_* */
	uint32_t     flags;       /* CF_* */
	enum win_type type;
	uint8_t      layer;       /* LAYER_* (実効レイヤ) */
	uint32_t     desktop;     /* WM_ALL_DESKTOPS で全面表示 */

	uint16_t     border_orig; /* reparent 前の border_width。復元用 (§3.1.0) */
	uint8_t      initial_state; /* WM_HINTS.initial_state (Normal/Iconic) */
	uint16_t     unmap_pending; /* WM 起因の UnmapNotify を無視する数 (§3.8) */

	xcb_window_t transient_for;
	xcb_window_t group;

	/* CSD: _GTK_FRAME_EXTENTS (left, right, top, bottom) (§7.2) */
	uint16_t     gtk_extents[4];

	/* _NET_WM_SYNC_REQUEST (§7.3) */
	xcb_sync_counter_t sync_counter;
	xcb_sync_alarm_t   sync_alarm;
	uint64_t     sync_target;
	uint64_t     sync_sent_ms;
	enum sync_state sync_state;

	/*
	 * アイコン (§4.4.1)。所有者が違うので必ず分けて持つこと。
	 *   icon_pix/icon_mask     : WM が作った 16x16。FreePixmap する
	 *   wmh_icon_pix/_mask     : WM_HINTS 由来のクライアント所有 ID。
	 *                            絶対に解放しない（他プロセスの資源）
	 */
	xcb_pixmap_t icon_pix;
	xcb_pixmap_t icon_mask;
	xcb_pixmap_t wmh_icon_pix;
	xcb_pixmap_t wmh_icon_mask;

	/* タイトルと計測キャッシュ (§2.3.1, §4.5.2.1) */
	char         title[WM_TITLE_MAX];
	uint8_t      title_len;
	uint8_t      draw_glyphs;
	uint8_t      draw_ellipsis;
	uint16_t     caption_w_at_measure;

	uint32_t     user_time;   /* _NET_WM_USER_TIME (§3.6) */

	struct client *next, *prev;       /* スタック順（下→上） */
	struct client *focus_next;        /* MRU 順 */
};

/* ================================================================== *
 * モニタ (SPEC §3.5.2)
 * ================================================================== */

struct monitor {
	struct rect geom;      /* モニタ全域 */
	struct rect workarea;  /* strut を引いたモニタ別作業領域 */
	int16_t     cascade_x, cascade_y;  /* カスケード配置の現在位置 (§3.3.1) */
	char        name[32];
};

/* ================================================================== *
 * 設定 (SPEC §8)
 * ================================================================== */

enum focus_mode { FOCUS_CLICK = 0, FOCUS_SLOPPY };

struct binding {
	uint16_t     mods;
	xcb_keysym_t keysym;
	uint8_t      action;      /* ACT_* */
	char        *arg;         /* exec の引数。パース時に確保して常駐 */
};

/* キーバインドの動作 */
enum {
	ACT_NONE = 0, ACT_CLOSE, ACT_SWITCH_NEXT, ACT_SWITCH_PREV,
	ACT_MAXIMIZE, ACT_MINIMIZE, ACT_FULLSCREEN, ACT_WINDOW_MENU,
	ACT_MOVE_KB, ACT_RESIZE_KB, ACT_DESKTOP_NEXT, ACT_DESKTOP_PREV,
	ACT_SHOW_DESKTOP, ACT_START_MENU, ACT_EXEC, ACT_QUIT, ACT_COUNT
};

struct config {
	/* 外観 */
	uint32_t color[16];          /* THEME_* で索引。theme.c が定義 */
	char     font[256];
	uint8_t  scale;

	/* 挙動 */
	enum focus_mode focus_mode;
	bool     focus_raise;
	bool     drag_outline;
	uint16_t snap_distance;
	uint8_t  desktops;
	bool     taskbar;
	bool     tray;
	bool     taskbar_autohide;
	uint8_t  taskbar_position;
	bool     animate_minimize;
	bool     force_ssd;

	struct binding bindings[WM_MAX_BINDINGS];
	uint16_t n_bindings;
};

bool config_load(struct config *cfg);       /* 既定値を入れてからファイルを読む */
void config_defaults(struct config *cfg);
void config_free(struct config *cfg);

/* ================================================================== *
 * WM のグローバル状態
 * ================================================================== */

struct wm {
	xcb_connection_t *conn;
	xcb_screen_t     *screen;
	int               screen_num;
	xcb_window_t      root;
	xcb_window_t      check_win;   /* _NET_SUPPORTING_WM_CHECK */
	xcb_window_t      focus_win;   /* 1x1 InputOnly。行き先が無い時の受け皿 (§3.6) */
	xcb_visualid_t    visual;
	uint8_t           depth;

	struct client    *stack_bottom, *stack_top;  /* スタック順の双方向リスト */
	struct client    *focus_list;                /* MRU 順 */
	struct client    *focused;
	uint16_t          n_clients;

	struct monitor    monitors[WM_MAX_MONITORS];
	uint8_t           n_monitors;

	uint32_t          current_desktop;
	uint32_t          n_desktops;

	struct config     cfg;

	/* RandR */
	uint8_t           randr_base;
	bool              have_randr;
	/* XSync */
	uint8_t           sync_base;
	bool              have_sync;
	/* Shape / XFixes は任意 */
	uint8_t           shape_base;
	bool              have_shape;

	xcb_timestamp_t   last_time;   /* 直近に受けたイベントの時刻 (ICCCM 用) */
	bool              running;
	bool              restart;
	int               sig_pipe[2];

	uint64_t          last_drag_ms;   /* ペーシング (§3.4.1) */
};

extern struct wm wm;

/* ================================================================== *
 * util.c — スラブ・ログ・文字列 (SPEC §2.3.0, §2.3.1)
 * ================================================================== */

void  log_init(int verbose);
void  log_msg(const char *fmt, ...);
void  log_err(const char *fmt, ...);
#define LOG(...)  log_msg(__VA_ARGS__)
#define ERR(...)  log_err(__VA_ARGS__)

/* スラブ。チャンク 0 は BSS。上限超過・確保失敗で NULL を返す (§2.3.0) */
struct client *slab_alloc(void);
void           slab_free(struct client *c);
uint16_t       slab_count(void);

/*
 * UTF-8 → UCS-2 変換 + サニタイズ (§2.3.1)
 *   - 不正シーケンスは U+FFFD、必ず 1 バイト以上進む（無限ループ防止）
 *   - BMP 外は U+FFFD
 *   - C0/C1 制御文字と双方向オーバーライドは除去
 * out には最大 out_max 個の UCS-2 を書き、書いた数を返す。
 */
size_t utf8_to_ucs2(const char *src, size_t src_len,
                    uint16_t *out, size_t out_max);

/* UTF-8 のコードポイント境界で安全に切り詰めた長さを返す (§2.3.1) */
size_t utf8_truncate_len(const char *s, size_t len, size_t max);

/* ================================================================== *
 * wm.c — 初期化・マネージャセレクション・adopt
 * ================================================================== */

bool wm_init(int argc, char **argv);
void wm_shutdown(void);
void wm_scan_existing(void);       /* 起動時の adopt (Phase 1-2) */
bool wm_acquire_selection(bool replace);

/*
 * デスクトップ切替 (SPEC §3.8)
 * 切替は frame の map/unmap で行う。非表示面のウィンドウは WM_STATE=Normal の
 * まま保ち、_NET_WM_STATE_HIDDEN は立てない（最小化と区別するため）。
 * WM 起因の UnmapNotify は client->unmap_pending で吸収する。
 */
void desktop_switch(uint32_t desktop);
void client_set_desktop(struct client *c, uint32_t desktop);
bool client_visible_on(const struct client *c, uint32_t desktop);

/* ================================================================== *
 * client.c — クライアントのライフサイクル (SPEC §3.1, §3.1.0, §3.1.1)
 * ================================================================== */

struct client *client_manage(xcb_window_t win, bool adopting);
void client_unmanage(struct client *c, bool destroyed);
struct client *client_find(xcb_window_t w);        /* win または frame から引く */
struct client *client_find_by_frame(xcb_window_t f);

void client_apply_geometry(struct client *c);      /* frame と win を配置し直す */
void client_send_configure(struct client *c);      /* synthetic ConfigureNotify */
void client_set_state(struct client *c, uint32_t wm_state);  /* WM_STATE */
void client_iconify(struct client *c);
void client_deiconify(struct client *c);
void client_close(struct client *c);               /* WM_DELETE_WINDOW か KillClient */
void client_update_frame_extents(struct client *c);
bool client_decides_decoration(struct client *c);  /* 装飾の有無を再判定 (§3.2) */

/*
 * 可視矩形 V を返す (SPEC §7.2)。
 *   SSD: フレームの外形    CSD: ウィンドウ矩形から _GTK_FRAME_EXTENTS を差し引いた矩形
 * 配置・スナップ・最大化・モニタ帰属判定はすべてこれを基準にする。
 * 各モジュールで個別に計算すると必ず食い違うため 1 箇所に集約する。
 */
void client_visual_rect(const struct client *c, struct rect *out);

/* 装飾の厚み。CSD/undecorated なら 0 を返す */
void client_frame_offsets(const struct client *c,
                          uint16_t *left, uint16_t *right,
                          uint16_t *top, uint16_t *bottom);

/* ================================================================== *
 * icccm.c — プロパティの読み取り (ICCCM)
 * ================================================================== */

void icccm_update_size_hints(struct client *c);
void icccm_update_wm_hints(struct client *c);
void icccm_update_protocols(struct client *c);
void icccm_update_transient(struct client *c);
void icccm_update_title(struct client *c);
void icccm_update_window_type(struct client *c);
void icccm_update_states(struct client *c);
void icccm_update_gtk_extents(struct client *c);

/* ConfigureRequest のジオメトリ計算。gravity を適用 (ICCCM §4.1.5) */
void icccm_apply_gravity(const struct client *c, uint8_t gravity,
                         uint16_t bw_old, uint16_t bw_new,
                         int16_t *x, int16_t *y);

/* ================================================================== *
 * stack.c — スタッキング (SPEC §3.7)
 * ================================================================== */

void stack_add(struct client *c);
void stack_remove(struct client *c);
void stack_raise(struct client *c);
void stack_lower(struct client *c);
void stack_apply(void);                    /* 実際の ConfigureWindow を発行 */
uint8_t stack_effective_layer(struct client *c);   /* 循環検出込み (§3.7.2) */
struct client *stack_transient_parent(struct client *c);
bool stack_is_transient_of(struct client *child, struct client *ancestor);
void stack_update_client_list(void);       /* _NET_CLIENT_LIST(_STACKING) */

/* ================================================================== *
 * focus.c — フォーカス (SPEC §3.6)
 * ================================================================== */

void focus_set(struct client *c, xcb_timestamp_t time);
void focus_none(void);                     /* focus_win へ退避 */
void focus_next_after(struct client *closing);
void focus_mru_promote(struct client *c);
void focus_mru_remove(struct client *c);
struct client *focus_mru_first(uint32_t desktop);

/* ================================================================== *
 * move.c — 移動・リサイズ (SPEC §3.4, §3.4.1)
 * ================================================================== */

enum drag_kind { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE };

/* edge は XCB_... ではなく独自ビット: 1=左 2=右 4=上 8=下 */
enum { EDGE_L = 1, EDGE_R = 2, EDGE_T = 4, EDGE_B = 8 };

void move_begin(struct client *c, enum drag_kind kind, uint8_t edge,
                int16_t root_x, int16_t root_y, xcb_timestamp_t time);
void move_motion(int16_t root_x, int16_t root_y);
void move_end(bool cancel);
bool move_active(void);
void move_tick(uint64_t now_ms);           /* poll のタイムアウトから呼ぶ */
/*
 * ドラッグ中のクライアントが破棄されるときに必ず呼ぶ。
 * これが無いと client_unmanage → slab_free 後のメモリをドラッグ処理が
 * 参照し続ける（解放済み領域が読めてしまうため症状が出にくい）。
 */
void move_forget(struct client *c);
int  move_next_timeout_ms(uint64_t now_ms); /* -1 なら待ちなし */

/* ================================================================== *
 * layout.c — モニタと作業領域 (SPEC §3.5.2, §3.3.1)
 * ================================================================== */

void layout_update_monitors(void);
struct monitor *layout_monitor_at(int16_t x, int16_t y);
struct monitor *layout_monitor_for(const struct client *c);
void layout_update_workareas(void);        /* strut を集めて再計算 */
void layout_place_new(struct client *c);   /* 初期配置 (§3.3.1) */
void layout_maximize(struct client *c, bool vert, bool horz, bool on);
void layout_fullscreen(struct client *c, bool on);

/* ================================================================== *
 * input.c — キー・マウスの grab (SPEC §6)
 * ================================================================== */

void input_init(void);
void input_regrab_keys(void);              /* MappingNotify で呼ぶ */
bool input_handle_key(xcb_key_press_event_t *ev);
void input_run_action(uint8_t action, const char *arg, struct client *c);
void spawn_command(const char *cmd);       /* posix_spawn (§2.2.1) */

/* ================================================================== *
 * event.c — ディスパッチ
 * ================================================================== */

void event_dispatch(xcb_generic_event_t *ev);
void event_handle_error(xcb_generic_error_t *err);  /* (§2.2.2) */

/* ================================================================== *
 * ewmh.c — プロパティの書き出し
 * ================================================================== */

void ewmh_init(void);
void ewmh_update_supported(void);
void ewmh_update_desktop_props(void);
void ewmh_update_workarea(void);
void ewmh_update_active_window(void);
void ewmh_set_wm_state(struct client *c);
void ewmh_set_allowed_actions(struct client *c);
void ewmh_set_frame_extents(struct client *c);
bool ewmh_handle_client_message(xcb_client_message_event_t *ev);

/* ================================================================== *
 * 小さなヘルパ（プロパティ読み取り）
 * ================================================================== */

/* 単一の CARDINAL/ATOM/WINDOW を読む。取れなければ false */
bool prop_get_card32(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type,
                     uint32_t *out);
/* 配列を読む。len には要素数を返す。呼び出し側が free する */
uint32_t *prop_get_card32_list(xcb_window_t w, xcb_atom_t prop,
                               xcb_atom_t type, uint32_t *len);
/* テキストを読み UTF-8 として buf に収める。戻り値は長さ */
size_t prop_get_text(xcb_window_t w, xcb_atom_t prop, char *buf, size_t buflen);

#endif /* W98WM_H */
