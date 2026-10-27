#!/bin/sh
#
# 075-cjk-title.sh
#
# 日本語タイトルの表示（SPEC §2.3.1, §4.5.4, §4.5.5）。
#
# 外部テスト（2026-10-27）で見つかった 2 つの不具合の回帰テスト:
#
#   1. **日本語フォントが入っていても豆腐になっていた。**
#      欧文の iso10646-1 フォント（7x13 等）は「範囲としては U+4E00 を
#      含む」ので、CJK 補助フォントを開かずに済ませていた。実際には
#      グリフが無く、タイトルもスタートボタンも □ の列になっていた。
#      jisx0208.1983-0 のフォント（k14 等）は区点で引く必要があるのに
#      UCS のまま渡していたので、こちらも使えていなかった。
#
#   2. **COMPOUND_TEXT のタイトルが化けていた。**
#      `xterm -T 日本語タイトル` の WM_NAME は COMPOUND_TEXT 型
#      （ESC $ ( B ... の ISO 2022）で、これを Latin-1 として読んでいた。
#
# 検証は「同じ文字列を別の経路で渡したら同じ絵になるか」で行う:
#   A: _NET_WM_NAME (UTF-8) に「日本語」
#   B: WM_NAME (COMPOUND_TEXT) に「日本語」     → A と同じ絵 (復号が正しい)
#   C: _NET_WM_NAME (UTF-8) に「本日語」       → A と違う絵 (豆腐なら同じになる)
#
# これを 2 つのフォント環境で行う:
#   default : 既定のフォントパス（xfonts-base の 12x13ja が補助になる）
#   jis     : 7x13 + k14 だけのフォントパス（補助は jisx0208 = 区点で引く経路）
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

PHASE="${1:-}"

# ------------------------------------------------------------------
# 親: 2 つのフォント環境で自分を呼び直す（Xvfb はそれぞれ別）
# ------------------------------------------------------------------
if [ -z "$PHASE" ]; then
	require_tool xwd
	require_tool convert
	command -v cc >/dev/null 2>&1 || skip "cc がありません"
	pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"

	MISC=/usr/share/fonts/X11/misc
	ran=0
	for ph in default jis; do
		case "$ph" in
		default)
			ls "$MISC"/12x13ja.pcf* >/dev/null 2>&1 || {
				echo "# default: 12x13ja が無いので省略 (xfonts-base)"
				continue
			}
			sh "$0" default && rc=0 || rc=$?
			;;
		jis)
			ls "$MISC"/k14.pcf* "$MISC"/7x13.pcf* "$MISC"/cursor.pcf* >/dev/null 2>&1 || {
				echo "# jis: k14 / 7x13 / cursor のどれかが無いので省略 (xfonts-intl-japanese 等)"
				continue
			}
			FD=$(mktemp -d)
			# ★ シンボリックリンクでは Xvfb がフォントを開けない（一覧には出るのに
			#   ListFontsWithInfo が 0 件になる）。実体を写す。
			cp "$MISC"/k14.pcf* "$MISC"/7x13.pcf* "$MISC"/cursor.pcf* "$FD"/
			if ! command -v mkfontdir >/dev/null 2>&1 || ! mkfontdir "$FD" 2>/dev/null; then
				rm -rf "$FD"
				echo "# jis: mkfontdir が無いので省略 (xfonts-utils)"
				continue
			fi
			XVFB_EXTRA_ARGS="-fp $FD/" sh "$0" jis && rc=0 || rc=$?
			rm -rf "$FD"
			;;
		esac
		[ "$rc" -eq 77 ] && { echo "# $ph: スキップ"; continue; }
		[ "$rc" -eq 0 ] || exit "$rc"
		ran=$((ran + 1))
	done
	[ "$ran" -gt 0 ] || skip "日本語のコアフォントが 1 つも無い"
	echo "# 日本語タイトル (UTF-8 / COMPOUND_TEXT) が CJK 補助フォントで描かれることを確認 ($ran 環境)"
	exit 0
fi

# ------------------------------------------------------------------
# 子: 1 つのフォント環境での検証
# ------------------------------------------------------------------
TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP" 2>/dev/null || true; }
# trap は後勝ち。lib.sh の _lib_cleanup と必ず合成する
trap '_lib_cleanup; cleanup_tmp' EXIT INT TERM

cat >"$TMP/titleprobe.c" <<'EOF'
/*
 * titleprobe <utf8|ctext> <x> <y> <タイトルの 16 進バイト列>
 *   utf8 : _NET_WM_NAME (UTF8_STRING) だけを置く
 *   ctext: WM_NAME (COMPOUND_TEXT) だけを置く
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t *c, const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w;
	uint32_t vals[1], h[18];
	unsigned char t[128];
	size_t n = 0;
	const char *hx;
	int16_t x, y;

	if (argc < 5 || xcb_connection_has_error(c))
		return 1;
	x = (int16_t)atoi(argv[2]);
	y = (int16_t)atoi(argv[3]);
	for (hx = argv[4]; hx[0] && hx[1] && n < sizeof t; hx += 2) {
		unsigned v;
		sscanf(hx, "%2x", &v);
		t[n++] = (unsigned char)v;
	}

	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	w = xcb_generate_id(c);
	vals[0] = 0xffffff;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, x, y, 300, 60, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, XCB_CW_BACK_PIXEL, vals);
	if (strcmp(argv[1], "ctext") == 0)
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
			atom(c, "COMPOUND_TEXT"), 8, (uint32_t)n, t);
	else
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom(c, "_NET_WM_NAME"),
			atom(c, "UTF8_STRING"), 8, (uint32_t)n, t);
	memset(h, 0, sizeof h);
	h[0] = 1u | 2u;                     /* USPosition | USSize (重ねない) */
	h[1] = (uint32_t)x; h[2] = (uint32_t)y; h[3] = 300; h[4] = 60;
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
	xcb_map_window(c, w);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	printf("WIN %u\n", w);
	fflush(stdout);
	for (;;) {
		xcb_generic_event_t *ev = xcb_wait_for_event(c);
		if (ev == NULL)
			break;
		free(ev);
	}
	return 0;
}
EOF
cc -std=c99 -o "$TMP/titleprobe" "$TMP/titleprobe.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb

# --- フォント選択: CJK 補助フォントを開いていること ---
pf=$("$WM_BIN" --print-font 2>/dev/null) || fail "[$PHASE] --print-font が失敗しました"
echo "# [$PHASE] $pf"
case "$PHASE" in
default)
	echo "$pf" | grep -q "+ aux .*-ja-" ||
		fail "[$PHASE] 12x13ja があるのに CJK 補助フォントを開いていません: $pf"
	;;
jis)
	echo "$pf" | grep -q "+ aux .*jisx0208" ||
		fail "[$PHASE] k14 (jisx0208) があるのに CJK 補助フォントを開いていません: $pf"
	;;
esac

start_wm

launch() {   # launch <名前> <utf8|ctext> <x> <y> <hex>
	"$TMP/titleprobe" "$2" "$3" "$4" "$5" >"$TMP/$1.out" 2>&1 &
	_LIB_PIDS="$_LIB_PIDS $!"
	i=0
	while [ $i -lt 50 ]; do
		w=$(awk '/^WIN /{print $2; exit}' "$TMP/$1.out" 2>/dev/null)
		[ -n "$w" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	[ -n "$w" ] || fail "[$PHASE] 検証クライアント $1 が窓を作れませんでした"
	i=0
	while [ $i -lt 50 ]; do
		f=$(get_parent_window "$w")
		[ -n "$f" ] && [ "$f" != "$(to_hex "$w")" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	echo "$w"
}

NIHONGO_UTF8=e697a5e69cace8aa9e                   # 日本語
HONJITSU_UTF8=e69cace697a5e8aa9e                  # 本日語
NIHONGO_CTEXT=1b242842467c4b5c386c1b2842          # ESC $ ( B 日本語 ESC ( B

wa=$(launch a utf8  100 100 "$NIHONGO_UTF8")
wb=$(launch b ctext 100 260 "$NIHONGO_CTEXT")
wc=$(launch c utf8  100 420 "$HONJITSU_UTF8")
# 最後に ASCII の窓を開いてアクティブを奪わせ、A/B/C を同じ非アクティブ色に揃える
wd=$(launch d utf8 600 100 "7a7a")
i=0
while [ $i -lt 50 ]; do
	xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null |
		grep -qi "$(printf '0x%x' "$wd")" && break
	sleep 0.2
	i=$((i + 1))
done
sleep 0.5

#
# キャプションの文字の部分だけを切り出して比べる。
# 3 枚とも同じ幅・同じ非アクティブ色なので、文字以外は画素単位で一致する。
#
caption_sig() {
	f=$(get_parent_window "$1")
	xwd -display "$DISPLAY" -id "$f" -silent -out "$TMP/cap.xwd" 2>/dev/null || { echo none; return; }
	[ -n "${CAP_DUMP:-}" ] && convert "$TMP/cap.xwd" -crop 140x14+6+6 +repage -scale 400% "$CAP_DUMP-$1.png" 2>/dev/null
	convert "$TMP/cap.xwd" -crop 140x14+6+6 +repage rgb:- 2>/dev/null | cksum | awk '{print $1}'
}
sa=$(caption_sig "$wa")
sb=$(caption_sig "$wb")
sc=$(caption_sig "$wc")
[ "$sa" != none ] && [ -n "$sa" ] || fail "[$PHASE] キャプションを撮れません"

[ "$sa" = "$sb" ] ||
	fail "[$PHASE] COMPOUND_TEXT の「日本語」が UTF-8 の「日本語」と違う絵です（復号できていない）"
[ "$sa" != "$sc" ] ||
	fail "[$PHASE] 「日本語」と「本日語」が同じ絵です（字形が出ず豆腐になっている）"

echo "# [$PHASE] UTF-8 と COMPOUND_TEXT が同じ字形、別の字は別の字形"
exit 0
