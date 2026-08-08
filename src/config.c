/*
 * config.c - 設定ファイルの探索・パースと既定値 (SPEC §8, §6)
 *
 * 方針:
 *   - config_load() は必ず config_defaults() を先に適用してから、見つかった
 *     設定ファイルで上書きする。ファイルが見つからないのはエラーではない。
 *   - パーサは行単位。1 行を 512B のスタックバッファに読み、そのバッファを
 *     その場で破壊的に分割する（'=' や空白の位置に '\0' を書き込む）。
 *     ヒープ確保はしない（SPEC §8: 「パーサは行単位・スタックバッファ 512B」）。
 *     唯一の例外は `bind = ... exec <cmd>` の <cmd> 文字列で、これは
 *     struct binding.arg として常駐するため strdup する（config_free で解放）。
 *   - 未知のキーは log_err で警告するだけで致命的にはしない。
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp (POSIX) */
#include <unistd.h>    /* access */

#include "w98wm.h"

/*
 * X11/keysymdef.h はキーシンボルの数値マクロ (XK_*) だけを提供するヘッダで、
 * 関数もライブラリのシンボルも定義しない。xcb-keysyms 自体はキー名の定数を
 * 一切持たないため、純粋 xcb ベースの実装でもこのヘッダだけを取り込むのが
 * 通例（libX11 へのリンクは発生しない）。
 */
#define XK_MISCELLANY 1  /* Tab / Esc / Space / 矢印 / Super などを含める */
#define XK_LATIN1     1  /* 英数字 1 文字の Latin-1 keysym を含める */
#include <X11/keysymdef.h>

/* ================================================================== *
 * アクション名 <-> ACT_* (§8 の bind 行、§6 の既定バインド)
 * ================================================================== */

struct action_name {
	const char *name;
	uint8_t     action;
};

static const struct action_name ACTION_NAMES[] = {
	{ "close",        ACT_CLOSE },
	{ "switch-next",  ACT_SWITCH_NEXT },
	{ "switch-prev",  ACT_SWITCH_PREV },
	{ "maximize",     ACT_MAXIMIZE },
	{ "minimize",     ACT_MINIMIZE },
	{ "fullscreen",   ACT_FULLSCREEN },
	{ "window-menu",  ACT_WINDOW_MENU },
	{ "move-kb",      ACT_MOVE_KB },
	{ "resize-kb",    ACT_RESIZE_KB },
	{ "desktop-next", ACT_DESKTOP_NEXT },
	{ "desktop-prev", ACT_DESKTOP_PREV },
	{ "show-desktop", ACT_SHOW_DESKTOP },
	{ "start-menu",   ACT_START_MENU },
	{ "exec",         ACT_EXEC },
	{ "quit",         ACT_QUIT },
};
#define N_ACTION_NAMES (sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]))

/* ================================================================== *
 * キー名 <-> keysym（§6 に出てくる名前のみの小さな静的表）
 * ================================================================== */

struct key_name {
	const char  *name;
	xcb_keysym_t keysym;
};

static const struct key_name KEY_NAMES[] = {
	{ "F1", XK_F1 }, { "F2", XK_F2 }, { "F3", XK_F3 }, { "F4", XK_F4 },
	{ "F5", XK_F5 }, { "F6", XK_F6 }, { "F7", XK_F7 }, { "F8", XK_F8 },
	{ "F9", XK_F9 }, { "F10", XK_F10 }, { "F11", XK_F11 }, { "F12", XK_F12 },
	{ "Tab",    XK_Tab },
	{ "Esc",    XK_Escape },
	{ "Escape", XK_Escape },
	{ "Space",  XK_space },
	{ "Left",   XK_Left },
	{ "Right",  XK_Right },
	{ "Up",     XK_Up },
	{ "Down",   XK_Down },
	/* Super 単体（修飾キーを押しっぱなしにせず単独で押す）のためのエイリアス。
	 * 修飾子の前置として使われた場合は resolve_modifier() 側で処理するので
	 * ここに来るのは「キー」として単体で書かれた場合のみ (例: `bind = Super
	 * start-menu`)。物理的には左 Super を指すことが多いので Super_L に倒す。 */
	{ "Super",   XK_Super_L },
	{ "Super_L", XK_Super_L },
	{ "Super_R", XK_Super_R },
};
#define N_KEY_NAMES (sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]))

static bool resolve_keysym(const char *name, xcb_keysym_t *out)
{
	for (size_t i = 0; i < N_KEY_NAMES; i++) {
		if (strcasecmp(name, KEY_NAMES[i].name) == 0) {
			*out = KEY_NAMES[i].keysym;
			return true;
		}
	}

	/* 単一の ASCII 英数字は Latin-1 keysym にそのまま対応する
	 * (XK_a..XK_z == 0x61..0x7a, XK_0..XK_9 == 0x30..0x39)。
	 * 物理キーが生成する既定 (シフト無し) のキーシンボルは小文字側なので、
	 * 英字は小文字に正規化する。 */
	if (name[0] != '\0' && name[1] == '\0') {
		unsigned char c = (unsigned char)name[0];
		if (isalpha(c))
			c = (unsigned char)tolower(c);
		if (isalnum(c)) {
			*out = (xcb_keysym_t)c;
			return true;
		}
	}

	return false;
}

static bool resolve_modifier(const char *tok, uint16_t *out)
{
	if (strcasecmp(tok, "shift") == 0) {
		*out = XCB_MOD_MASK_SHIFT;
		return true;
	}
	if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) {
		*out = XCB_MOD_MASK_CONTROL;
		return true;
	}
	if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0) {
		*out = XCB_MOD_MASK_1;
		return true;
	}
	if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "mod4") == 0) {
		*out = XCB_MOD_MASK_4;
		return true;
	}
	return false;
}

/* "Ctrl+Alt+Left" のような文字列を '+' で分割する。s を破壊的に書き換える。
 * 戻り値はトークン数（0 は空文字列）。 */
static int split_plus(char *s, char *tokens[], int max_tokens)
{
	int n = 0;
	char *p = s;

	if (*p == '\0')
		return 0;

	while (n < max_tokens) {
		tokens[n++] = p;
		char *plus = strchr(p, '+');
		if (!plus)
			break;
		*plus = '\0';
		p = plus + 1;
	}
	return n;
}

static bool parse_bind_spec(char *spec, uint16_t *mods_out, xcb_keysym_t *keysym_out)
{
	char *tokens[6];
	int n = split_plus(spec, tokens, 6);
	if (n <= 0)
		return false;

	uint16_t mods = 0;
	for (int i = 0; i < n - 1; i++) {
		uint16_t m;
		if (!resolve_modifier(tokens[i], &m))
			return false;
		mods |= m;
	}

	xcb_keysym_t ks;
	if (!resolve_keysym(tokens[n - 1], &ks))
		return false;

	*mods_out = mods;
	*keysym_out = ks;
	return true;
}

/* ================================================================== *
 * バインド配列への追加（既定値・ファイル由来の両方から呼ばれる）
 * ================================================================== */

static void add_binding(struct config *cfg, uint16_t mods, xcb_keysym_t keysym,
                        uint8_t action, char *arg)
{
	if (cfg->n_bindings >= WM_MAX_BINDINGS) {
		ERR("config: bind の上限 (%d) を超えました。以降の bind は無視します",
		    WM_MAX_BINDINGS);
		free(arg);
		return;
	}
	struct binding *b = &cfg->bindings[cfg->n_bindings++];
	b->mods = mods;
	b->keysym = keysym;
	b->action = action;
	b->arg = arg;
}

/* config_defaults 専用の小さなラッパ。既定バインドは exec を持たないので
 * arg は常に NULL。 */
static void add_default_binding(struct config *cfg, uint16_t mods,
                                xcb_keysym_t keysym, uint8_t action)
{
	add_binding(cfg, mods, keysym, action, NULL);
}

/* ================================================================== *
 * config_defaults (SPEC §8 既定値表, §6 既定キー/マウス操作表)
 * ================================================================== */

void config_defaults(struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	/* ---- 外観 ---- *
	 * color[16] は THEME_* で索引される配列だが、その enum は theme.c
	 * (Phase 2) が定義する。ここでは絶対に色定数を捏造しない。ゼロ初期化
	 * のまま theme.c 側の初期化に委ねる（上の memset で既にゼロ）。 */
	strlcpy(cfg->font,
	        "-*-helvetica-medium-r-normal--11-*-*-*-p-*-iso10646-1",
	        sizeof(cfg->font));
	cfg->scale = 1;

	/* ---- 挙動 ---- */
	cfg->focus_mode       = FOCUS_CLICK;
	cfg->focus_raise      = true;
	cfg->drag_outline     = false;
	cfg->snap_distance    = 8;
	cfg->desktops         = 1;
	cfg->taskbar          = true;
	cfg->tray             = true;
	cfg->taskbar_autohide = false;
	cfg->taskbar_position = 0; /* 0=bottom, 1=top, 2=left, 3=right
	                            * (taskbar.c 側の enum が無いためここで
	                            * エンコーディングを定義する) */
	cfg->animate_minimize = true;
	cfg->force_ssd         = false; /* §7.2: 既定では CSD を尊重する */

	/* ---- 既定キーバインド (SPEC §6) ---- *
	 * マウス操作 (Alt+左/右ドラッグ) は struct binding が持てない
	 * (keysym のみでボタン番号を持たない) ので input.c が直接 grab する。 */

	/* Alt+F4: 閉じる */
	add_default_binding(cfg, XCB_MOD_MASK_1, XK_F4, ACT_CLOSE);
	/* Alt+Tab / Alt+Shift+Tab: タスクスイッチャ（次/前） */
	add_default_binding(cfg, XCB_MOD_MASK_1, XK_Tab, ACT_SWITCH_NEXT);
	add_default_binding(cfg, (uint16_t)(XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT),
	                    XK_Tab, ACT_SWITCH_PREV);
	/* Alt+Space: ウィンドウメニュー */
	add_default_binding(cfg, XCB_MOD_MASK_1, XK_space, ACT_WINDOW_MENU);
	/* Alt+F7 / Alt+F8: キーボードによる移動 / リサイズ */
	add_default_binding(cfg, XCB_MOD_MASK_1, XK_F7, ACT_MOVE_KB);
	add_default_binding(cfg, XCB_MOD_MASK_1, XK_F8, ACT_RESIZE_KB);
	/* Ctrl+Alt+Left/Right: デスクトップ切替（Left=前, Right=次） */
	add_default_binding(cfg, (uint16_t)(XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1),
	                    XK_Left, ACT_DESKTOP_PREV);
	add_default_binding(cfg, (uint16_t)(XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1),
	                    XK_Right, ACT_DESKTOP_NEXT);
	/* Ctrl+Esc: スタートメニュー */
	add_default_binding(cfg, XCB_MOD_MASK_CONTROL, XK_Escape, ACT_START_MENU);
	/* Super 単体（左右どちらでも）: スタートメニュー。修飾子無し・キー自体が
	 * Super_L / Super_R という grab になる（押しっぱなしではなく単独押下）。 */
	add_default_binding(cfg, 0, XK_Super_L, ACT_START_MENU);
	add_default_binding(cfg, 0, XK_Super_R, ACT_START_MENU);
	/* Super+D: デスクトップの表示/復元 */
	add_default_binding(cfg, XCB_MOD_MASK_4, XK_d, ACT_SHOW_DESKTOP);
}

/* ================================================================== *
 * 値パーサ
 * ================================================================== */

static bool parse_bool(const char *v, bool *out)
{
	if (strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
	    strcmp(v, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
	    strcmp(v, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static bool parse_long(const char *v, long min, long max, long *out)
{
	if (*v == '\0')
		return false;
	char *end;
	errno = 0;
	long val = strtol(v, &end, 10);
	if (end == v || *end != '\0' || errno != 0)
		return false;
	if (val < min || val > max)
		return false;
	*out = val;
	return true;
}

static bool is_color_value(const char *v)
{
	if (v[0] != '#')
		return false;
	for (int i = 1; i <= 6; i++) {
		if (v[i] == '\0' || !isxdigit((unsigned char)v[i]))
			return false;
	}
	return v[7] == '\0';
}

/* ================================================================== *
 * bind = <MODS+KEY> <action> [arg]
 * ================================================================== */

static char *next_token(char **pp)
{
	char *p = *pp;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0') {
		*pp = p;
		return NULL;
	}
	char *start = p;
	while (*p != '\0' && *p != ' ' && *p != '\t')
		p++;
	if (*p != '\0') {
		*p = '\0';
		p++;
	}
	*pp = p;
	return start;
}

static void parse_bind(struct config *cfg, char *value)
{
	char *p = value;
	char *spec_tok = next_token(&p);
	char *action_tok = next_token(&p);

	if (!spec_tok || !action_tok) {
		ERR("config: bind の書式が不正です: '%s'", value);
		return;
	}

	while (*p == ' ' || *p == '\t')
		p++;
	const char *arg = (*p != '\0') ? p : NULL;

	uint16_t mods;
	xcb_keysym_t keysym;
	if (!parse_bind_spec(spec_tok, &mods, &keysym)) {
		ERR("config: bind の修飾子/キー名が不明です: '%s'", spec_tok);
		return;
	}

	uint8_t action = ACT_NONE;
	bool found = false;
	for (size_t i = 0; i < N_ACTION_NAMES; i++) {
		if (strcasecmp(action_tok, ACTION_NAMES[i].name) == 0) {
			action = ACTION_NAMES[i].action;
			found = true;
			break;
		}
	}
	if (!found) {
		ERR("config: bind の動作名が不明です: '%s'", action_tok);
		return;
	}

	char *arg_dup = NULL;
	if (action == ACT_EXEC) {
		if (!arg) {
			ERR("config: exec に引数がありません: 'bind = %s %s'",
			    spec_tok, action_tok);
			return;
		}
		arg_dup = strdup(arg);
		if (!arg_dup) {
			ERR("config: strdup に失敗しました（exec の bind をスキップ）");
			return;
		}
	}

	add_binding(cfg, mods, keysym, action, arg_dup);
}

/* ================================================================== *
 * key = value の適用（§8 のキー名を正とする）
 * ================================================================== */

static void apply_key(struct config *cfg, const char *key, char *value)
{
	bool b;
	long n;

	if (strcmp(key, "theme") == 0) {
		static const char *const themes[] = {
			"standard", "rainy", "eggplant", "plum", "hicontrast", "custom"
		};
		bool ok = false;
		for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++) {
			if (strcmp(value, themes[i]) == 0) {
				ok = true;
				break;
			}
		}
		if (!ok)
			ERR("config: theme の値が不正です: '%s'", value);
		/* struct config には theme 名を保持するフィールドが無い
		 * (theme.c 未実装、Phase 2 待ち)。値の妥当性検証のみ行い、
		 * 格納は行わない。header 側にフィールドが追加され次第対応する。 */
		return;
	}

	if (strncmp(key, "color.", 6) == 0) {
		if (!is_color_value(value))
			ERR("config: %s の値が #RRGGBB 形式ではありません: '%s'", key, value);
		/* color[16] は THEME_* (theme.c 所有) で索引する配列であり、
		 * ここでインデックスを捏造しない。パレットの実際の適用は
		 * theme.c (Phase 2) の役目。ここでは書式検証のみ行う。 */
		return;
	}

	if (strcmp(key, "font") == 0) {
		strlcpy(cfg->font, value, sizeof(cfg->font));
		return;
	}

	if (strcmp(key, "scale") == 0) {
		if (parse_long(value, 1, 2, &n))
			cfg->scale = (uint8_t)n;
		else
			ERR("config: scale は 1 か 2 でなければなりません: '%s'", value);
		return;
	}

	if (strcmp(key, "focus.mode") == 0) {
		if (strcmp(value, "click") == 0)
			cfg->focus_mode = FOCUS_CLICK;
		else if (strcmp(value, "sloppy") == 0)
			cfg->focus_mode = FOCUS_SLOPPY;
		else
			ERR("config: focus.mode は click か sloppy でなければなりません: '%s'", value);
		return;
	}

	if (strcmp(key, "focus.raise") == 0) {
		if (parse_bool(value, &b))
			cfg->focus_raise = b;
		else
			ERR("config: focus.raise の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "drag.outline") == 0) {
		if (parse_bool(value, &b))
			cfg->drag_outline = b;
		else
			ERR("config: drag.outline の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "snap.distance") == 0) {
		if (parse_long(value, 0, 10000, &n))
			cfg->snap_distance = (uint16_t)n;
		else
			ERR("config: snap.distance の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "desktops") == 0) {
		if (parse_long(value, 1, WM_MAX_DESKTOPS, &n))
			cfg->desktops = (uint8_t)n;
		else
			ERR("config: desktops は 1..%d でなければなりません: '%s'",
			    WM_MAX_DESKTOPS, value);
		return;
	}

	if (strcmp(key, "taskbar") == 0) {
		if (parse_bool(value, &b))
			cfg->taskbar = b;
		else
			ERR("config: taskbar の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "taskbar.position") == 0) {
		uint8_t pos;
		if (strcmp(value, "bottom") == 0)
			pos = 0;
		else if (strcmp(value, "top") == 0)
			pos = 1;
		else if (strcmp(value, "left") == 0)
			pos = 2;
		else if (strcmp(value, "right") == 0)
			pos = 3;
		else {
			ERR("config: taskbar.position の値が不正です: '%s'", value);
			return;
		}
		cfg->taskbar_position = pos;
		return;
	}

	if (strcmp(key, "taskbar.autohide") == 0) {
		if (parse_bool(value, &b))
			cfg->taskbar_autohide = b;
		else
			ERR("config: taskbar.autohide の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "tray") == 0) {
		if (parse_bool(value, &b))
			cfg->tray = b;
		else
			ERR("config: tray の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "animate.minimize") == 0) {
		if (parse_bool(value, &b))
			cfg->animate_minimize = b;
		else
			ERR("config: animate.minimize の値が不正です: '%s'", value);
		return;
	}

	if (strcmp(key, "bind") == 0) {
		parse_bind(cfg, value);
		return;
	}

	ERR("config: 未知のキーです（無視します）: '%s'", key);
}

/* ================================================================== *
 * 行のトリムとパース
 * ================================================================== */

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	if (*s == '\0')
		return s;
	char *end = s + strlen(s) - 1;
	while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		end--;
	end[1] = '\0';
	return s;
}

static void parse_line(struct config *cfg, char *line, int lineno)
{
	char *t = trim(line);
	if (*t == '\0' || *t == '#')
		return; /* 空行・コメント行 (§8) */

	char *eq = strchr(t, '=');
	if (!eq) {
		ERR("config: %d 行目: '=' がありません（無視します）: '%s'", lineno, t);
		return;
	}
	*eq = '\0';
	char *key = trim(t);
	char *value = trim(eq + 1);

	if (*key == '\0') {
		ERR("config: %d 行目: キー名が空です（無視します）", lineno);
		return;
	}

	apply_key(cfg, key, value);
}

/* 512B のスタックバッファでファイルを読む (SPEC §8)。
 * バッファに収まらない行は警告のうえ、残りを読み捨てて次の行へ進む
 * （残りを新しい行として誤ってパースしない）。 */
static void parse_stream(struct config *cfg, FILE *fp)
{
	char line[512];
	int lineno = 0;

	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		size_t len = strlen(line);
		bool no_newline = (len == 0) || (line[len - 1] != '\n');
		bool filled = (len == sizeof(line) - 1);

		if (no_newline && filled && !feof(fp)) {
			/* 512B に収まらない行。警告して残りを読み捨てる */
			ERR("config: %d 行目: 行が長すぎます（512B 超）。スキップします", lineno);
			int c;
			while ((c = fgetc(fp)) != EOF && c != '\n')
				;
			continue;
		}

		parse_line(cfg, line, lineno);
	}
}

/* ================================================================== *
 * 設定ファイルの探索 (SPEC §8)
 * ================================================================== */

static bool try_dir(char *out, size_t outsz, const char *dir, size_t dirlen)
{
	if (dirlen == 0)
		return false;
	int n = snprintf(out, outsz, "%.*s/%s/%s",
	                 (int)dirlen, dir, WM_CONFIG_DIR, WM_CONFIG_FILE);
	if (n < 0 || (size_t)n >= outsz)
		return false;
	return access(out, R_OK) == 0;
}

static bool find_config_path(char *out, size_t outsz)
{
	const char *xdg_home = getenv("XDG_CONFIG_HOME");
	if (xdg_home != NULL && xdg_home[0] != '\0') {
		if (try_dir(out, outsz, xdg_home, strlen(xdg_home)))
			return true;
	} else {
		const char *home = getenv("HOME");
		if (home != NULL && home[0] != '\0') {
			char buf[600];
			int n = snprintf(buf, sizeof(buf), "%s/.config", home);
			if (n > 0 && (size_t)n < sizeof(buf) &&
			    try_dir(out, outsz, buf, strlen(buf)))
				return true;
		}
	}

	const char *xdg_dirs = getenv("XDG_CONFIG_DIRS");
	if (xdg_dirs != NULL && xdg_dirs[0] != '\0') {
		char dirs_buf[1024];
		strlcpy(dirs_buf, xdg_dirs, sizeof(dirs_buf));
		char *saveptr = NULL;
		for (char *tok = strtok_r(dirs_buf, ":", &saveptr); tok != NULL;
		     tok = strtok_r(NULL, ":", &saveptr)) {
			if (try_dir(out, outsz, tok, strlen(tok)))
				return true;
		}
	}

	if (access(WM_SYSCONF_PATH, R_OK) == 0) {
		strlcpy(out, WM_SYSCONF_PATH, outsz);
		return true;
	}

	return false;
}

/* ================================================================== *
 * 公開 API
 * ================================================================== */

bool config_load(struct config *cfg)
{
	config_defaults(cfg);

	char path[768];
	if (!find_config_path(path, sizeof(path)))
		return true; /* 見つからないのはエラーではない (§8) */

	FILE *fp = fopen(path, "r");
	if (!fp) {
		/* access() 通過後の open 失敗は稀（レース等）。致命扱いにはしない */
		ERR("config: '%s' を開けません: %s", path, strerror(errno));
		return true;
	}

	LOG("config: '%s' を読み込みます", path);
	parse_stream(cfg, fp);
	fclose(fp);
	return true;
}

void config_free(struct config *cfg)
{
	for (uint16_t i = 0; i < cfg->n_bindings; i++) {
		free(cfg->bindings[i].arg);
		cfg->bindings[i].arg = NULL;
	}
	cfg->n_bindings = 0;
}
