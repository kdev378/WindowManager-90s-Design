/*
 * brand.h - 名称に関わる文字列の一元定義（SPEC §11.1）
 *
 * 名称は実装が動いてから決める。それまでの仮称が各所に散らないよう、
 * 名前に由来する文字列はすべてここを経由して参照する。
 * ソース中に仮称のリテラルを直接書いてはならない（CI が grep で検査する）。
 */
#ifndef BRAND_H
#define BRAND_H

/* 短い識別子。バイナリ名・設定ディレクトリ名・ログ接頭辞に使う。 */
#define WM_NAME         "w98wm"

/* 人間向けの表示名。_NET_WM_NAME（サポートチェックウィンドウ）等に出る。 */
#define WM_DISPLAY_NAME "w98wm"

/* WM_CLASS の instance / class */
#define WM_CLASS_INSTANCE "w98wm"
#define WM_CLASS_CLASS    "W98wm"

/* 独自アトムの接頭辞。例: _W98WM_FOO */
#define WM_ATOM_PREFIX  "_W98WM_"

/* 設定ファイルの探索に使うディレクトリ名（$XDG_CONFIG_HOME/<この名前>/config） */
#define WM_CONFIG_DIR   "w98wm"
#define WM_CONFIG_FILE  "config"
#define WM_SYSCONF_PATH "/etc/" WM_CONFIG_DIR "/" WM_CONFIG_FILE

#define WM_VERSION      "0.1.0-dev"

#endif /* BRAND_H */
