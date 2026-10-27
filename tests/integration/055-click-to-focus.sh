#!/bin/sh
#
# 055-click-to-focus.sh
#
# クリックでのアクティブ化と、キャプションの配色の追従（SPEC §3.6, §4.4）。
#
# 外部テスト（2026-10-27）で見つかった 2 つの不具合の回帰テスト:
#
#   1. **窓の中身をクリックしてもアクティブにならなかった。**
#      click-to-focus のパッシブ grab を張っていなかったため、
#      WM が受け取れるのはタイトルバーと枠のクリックだけだった。
#      既存テストは全部 `xdotool windowactivate`（= _NET_ACTIVE_WINDOW）で
#      切り替えていたので、一度も踏んでいなかった。
#
#   2. **アクティブが変わってもキャプションの色が変わらなかった。**
#      FocusIn で描き直す作りだったが、FOCUS_CHANGE を誰も select して
#      いなかったのでハンドラが一度も呼ばれていなかった。
#      070 は窓 1 枚のアクティブ色しか見ておらず、非アクティブ色を
#      検証するテストが無かった。
#
# 検証項目:
#   1. 後から開いた窓がアクティブ色、先の窓が非アクティブ色
#   2. 非アクティブな窓の**中身**をクリックするとアクティブになる
#   3. そのクリックが**アプリにも届いている**（ReplayPointer が効いている。
#      grab しただけで返さないと、フォーカスは移るがクリックが食われる）
#   4. アクティブになった窓の中のクリックも引き続きアプリに届く
#   5. _NET_ACTIVE_WINDOW 経由で切り替えても配色が入れ替わる
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"
require_tool xwd
require_tool convert
require_tool xwininfo

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP" 2>/dev/null || true; }
# ★ lib.sh の後始末と**合成**すること。trap は後勝ちなので、
#   'cleanup_tmp' だけを張ると Xvfb と WM が残る（外部テストで発見）。
trap '_lib_cleanup; cleanup_tmp' EXIT INT TERM

cat >"$TMP/clickprobe.c" <<'EOF'
/*
 * ButtonPress を自分で select する窓（xterm や GTK と同じ条件）。
 * 押されるたびに "BTN" を出す。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w;
	xcb_generic_event_t *ev;
	uint32_t vals[2];
	int16_t x = (int16_t)(argc > 2 ? atoi(argv[2]) : 40);
	int16_t y = (int16_t)(argc > 3 ? atoi(argv[3]) : 40);
	const char *name = argc > 1 ? argv[1] : "probe";

	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	w = xcb_generate_id(c);
	vals[0] = 0xffffff;
	vals[1] = XCB_EVENT_MASK_BUTTON_PRESS;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, x, y, 260, 160, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(name), name);
	{
		/*
		 * USPosition を立てて指定位置に置かせる。立てないと WM の
		 * カスケード配置で 2 枚目が 1 枚目に重なり、「alpha の中身」の
		 * つもりのクリックが beta に当たる（実際にそうなった）。
		 */
		uint32_t h[18];
		memset(h, 0, sizeof h);
		h[0] = 1u | 2u;                 /* USPosition | USSize */
		h[1] = (uint32_t)x; h[2] = (uint32_t)y;
		h[3] = 260; h[4] = 160;
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
			XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
	}
	xcb_map_window(c, w);
	xcb_flush(c);
	printf("WIN %u\n", w);
	fflush(stdout);

	while ((ev = xcb_wait_for_event(c)) != NULL) {
		if ((ev->response_type & 0x7f) == XCB_BUTTON_PRESS) {
			printf("BTN\n");
			fflush(stdout);
		}
		free(ev);
	}
	return 0;
}
EOF
cc -std=c99 -o "$TMP/clickprobe" "$TMP/clickprobe.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

launch() {   # launch <名前> <x> <y> → 窓 ID (10 進) を返す
	"$TMP/clickprobe" "$1" "$2" "$3" >"$TMP/$1.out" 2>&1 &
	_LIB_PIDS="$_LIB_PIDS $!"
	i=0
	while [ $i -lt 50 ]; do
		w=$(awk '/^WIN /{print $2; exit}' "$TMP/$1.out" 2>/dev/null)
		[ -n "$w" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	[ -n "$w" ] || fail "検証クライアント $1 が窓を作れませんでした"
	# 管理下に入り、アクティブになるまで待つ
	i=0
	while [ $i -lt 50 ]; do
		xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null |
			grep -qi "$(printf '0x%x' "$w")" && break
		sleep 0.2
		i=$((i + 1))
	done
	echo "$w"
}

active() {
	xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null | sed 's/.*# *//'
}
is_active() { [ "$(active)" = "$(printf '0x%x' "$1")" ]; }
n_btn() { grep -c '^BTN' "$TMP/$1.out" 2>/dev/null || true; }

frame_of() { get_parent_window "$1"; }

#
# キャプションの色を 1 点読む。frame を xwd で直接撮り、
# 最上段 (上枠 4px の直下) のボタン域より左の画素を見る。
# 結果を blue / gray / other に分類する。
#   アクティブ   : #000080 → #1084D0 のグラデーション (青が支配的)
#   非アクティブ : #808080 → #B5B5B5 のグラデーション (R≒G≒B)
#
caption_class() {
	f="$1"
	fw=$(xwininfo -display "$DISPLAY" -id "$f" 2>/dev/null | awk '/^  Width:/ {print $2}')
	xwd -display "$DISPLAY" -id "$f" -silent -out "$TMP/cap.xwd" 2>/dev/null || { echo none; return; }
	px=$(convert "$TMP/cap.xwd" -crop 1x1+$((fw - 70))+5 +repage txt:- 2>/dev/null |
		tail -1 | grep -o '#[0-9A-Fa-f]\{6\}' | head -1)
	[ -n "$px" ] || { echo none; return; }
	r=$(printf '%d' "0x$(echo "$px" | cut -c2-3)")
	g=$(printf '%d' "0x$(echo "$px" | cut -c4-5)")
	b=$(printf '%d' "0x$(echo "$px" | cut -c6-7)")
	if [ "$b" -gt $((r + 40)) ]; then
		echo blue
	elif [ $((r - b)) -lt 16 ] && [ $((b - r)) -lt 16 ] && [ $((r - g)) -lt 16 ] && [ $((g - r)) -lt 16 ] && [ "$r" -ge 96 ]; then
		echo gray
	else
		echo "other($px)"
	fi
}
wait_caption() {   # wait_caption <frame> <blue|gray>
	i=0
	while [ $i -lt 30 ]; do
		[ "$(caption_class "$1")" = "$2" ] && return 0
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

wa=$(launch alpha 40 40)
wb=$(launch beta 420 300)
fa=$(frame_of "$wa")
fb=$(frame_of "$wb")
[ -n "$fa" ] && [ -n "$fb" ] || fail "フレームが見つかりません (alpha=$fa beta=$fb)"

# ------------------------------------------------------------------
# 1. 後から開いた beta がアクティブ、alpha は非アクティブの色
# ------------------------------------------------------------------
is_active "$wb" || fail "(1) 後から開いた beta がアクティブではありません (active=$(active))"
wait_caption "$fb" blue || fail "(1) アクティブな beta のキャプションが青ではありません: $(caption_class "$fb")"
wait_caption "$fa" gray ||
	fail "(1) 非アクティブな alpha のキャプションが灰色ではありません: $(caption_class "$fa")（配色が追従していない）"

# ------------------------------------------------------------------
# 2〜3. alpha の**中身**をクリック → アクティブになり、クリックはアプリにも届く
# ------------------------------------------------------------------
geo=$(xwininfo -display "$DISPLAY" -id "$wa" 2>/dev/null)
ax=$(echo "$geo" | awk '/Absolute upper-left X/ {print $4}')
ay=$(echo "$geo" | awk '/Absolute upper-left Y/ {print $4}')
before=$(n_btn alpha)

xdotool mousemove $((ax + 100)) $((ay + 80)) click 1 >/dev/null 2>&1
i=0
while [ $i -lt 30 ]; do
	is_active "$wa" && break
	sleep 0.2
	i=$((i + 1))
done
is_active "$wa" ||
	fail "(2) 窓の中身をクリックしてもアクティブになりません (active=$(active))。click-to-focus の grab が張られていない"

i=0
while [ $i -lt 20 ]; do
	[ "$(n_btn alpha)" -gt "$before" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "$(n_btn alpha)" -gt "$before" ] ||
	fail "(3) フォーカスは移ったが、クリックがアプリに届いていません。grab したイベントを ReplayPointer で返していない"

wait_caption "$fa" blue || fail "(2) クリックでアクティブにした alpha が青になりません: $(caption_class "$fa")"
wait_caption "$fb" gray || fail "(2) 非アクティブになった beta が灰色になりません: $(caption_class "$fb")"

# ------------------------------------------------------------------
# 4. アクティブな窓の中のクリックもアプリに届く（grab は外れているはず）
# ------------------------------------------------------------------
before=$(n_btn alpha)
xdotool click 1 >/dev/null 2>&1
sleep 0.6
[ "$(n_btn alpha)" -gt "$before" ] ||
	fail "(4) アクティブな窓の中のクリックがアプリに届きません"

# ------------------------------------------------------------------
# 5. _NET_ACTIVE_WINDOW 経由の切り替えでも配色が入れ替わる
# ------------------------------------------------------------------
xdotool windowactivate "$wb" >/dev/null 2>&1 || true
i=0
while [ $i -lt 30 ]; do
	is_active "$wb" && break
	sleep 0.2
	i=$((i + 1))
done
is_active "$wb" || fail "(5) windowactivate で beta がアクティブになりません"
wait_caption "$fb" blue || fail "(5) beta が青になりません: $(caption_class "$fb")"
wait_caption "$fa" gray || fail "(5) alpha が灰色に戻りません: $(caption_class "$fa")"

echo "# クリックでのアクティブ化・クリックのアプリへの到達・キャプション配色の追従を確認"
exit 0
