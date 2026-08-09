#!/bin/sh
#
# 160-net-wm-state.sh
#
# `_NET_WM_STATE` の **状態遷移** を検証する（SPEC §5.2, EWMH）。
#
# なぜ「アトムが _NET_SUPPORTED にある」だけでは足りないか
# --------------------------------------------------------
# 010 は `_NET_SUPPORTED` にアトムが載っていることしか見ていない。
# クライアントが実際に使うのは
#
#   ClientMessage(_NET_WM_STATE, action, atom1, atom2, source)
#     ↓
#   WM 内部状態が変わる
#     ↓
#   ウィンドウのプロパティ / ジオメトリ / スタック順 / タスクバーが変わる
#
# という連鎖であって、その途中で切れていても
# 「_NET_SUPPORTED には載っている」テストは通ってしまう。
#
# 特に MAXIMIZED_VERT / MAXIMIZED_HORZ は分離されているので、
# **片軸を解除したときにもう片軸の値を壊さない**という条件が要る。
# ここは実アプリ（普通は両軸同時にしか使わない）では踏めない。
#
# 検証項目:
#   1. ADD / REMOVE / TOGGLE の 3 つのアクション
#   2. 1 つの ClientMessage で 2 つの状態を同時に指定
#   3. 未知のアトムを混ぜても、既知の方は処理される
#   4. 最大化の軸ごとの組み合わせ（縦のみ / 横のみ / 両方 / 片方だけ解除）
#   5. 最大化 → 移動 → 復元でジオメトリが戻る
#   6. ClientMessage の高速連打で状態が壊れない
#   7. SKIP_TASKBAR がタスクバーのボタンに反映される
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"
require_tool xterm

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/setstate.c" <<'EOF'
/*
 * _NET_WM_STATE のクライアントメッセージを送るだけの道具。
 *
 *   setstate <window> <add|remove|toggle> <atom名> [atom名2]
 *
 * wmctrl でも同じことはできるが、
 *   - 未知のアトムを混ぜる
 *   - source indication を指定する
 *   - 高速連打する
 * が wmctrl では書けないので専用に用意する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_client_message_event_t ev;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t st, a1 = 0, a2 = 0;
	uint32_t action;
	xcb_window_t w;

	if (argc < 4)
		return 2;
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	w = (xcb_window_t)strtoul(argv[1], NULL, 0);

	if (strcmp(argv[2], "remove") == 0)      action = 0;
	else if (strcmp(argv[2], "add") == 0)    action = 1;
	else if (strcmp(argv[2], "toggle") == 0) action = 2;
	else                                     action = (uint32_t)atoi(argv[2]);

	r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 14, "_NET_WM_STATE"), NULL);
	/* 長さは strlen で取り直す (上の 14 は仮) */
	free(r);
	r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen("_NET_WM_STATE"), "_NET_WM_STATE"), NULL);
	st = r ? r->atom : 0;
	free(r);

	r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(argv[3]), argv[3]), NULL);
	a1 = r ? r->atom : 0;
	free(r);

	if (argc > 4) {
		r = xcb_intern_atom_reply(c,
			xcb_intern_atom(c, 0, (uint16_t)strlen(argv[4]), argv[4]), NULL);
		a2 = r ? r->atom : 0;
		free(r);
	}

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = w;
	ev.format = 32;
	ev.type = st;
	ev.data.data32[0] = action;
	ev.data.data32[1] = a1;
	ev.data.data32[2] = a2;
	ev.data.data32[3] = 2;      /* source indication: pager (常に許可される) */
	ev.data.data32[4] = 0;

	xcb_send_event(c, 0, s->root,
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(const char *)&ev);
	xcb_flush(c);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/setstate" "$TMP/setstate.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

xterm -display "$DISPLAY" -geometry 50x16+120+120 >/dev/null 2>&1 &
XT=$!
_LIB_PIDS="$_LIB_PIDS $XT"
win=$(wait_for_managed_window_by_pid "$XT" 20) || fail "xterm が管理下に入りません"
sleep 1

wm_alive() { kill -0 "$WM_PID" 2>/dev/null; }
states() {
	xprop -display "$DISPLAY" -id "$win" _NET_WM_STATE 2>/dev/null | sed 's/.*= *//'
}
has_state() { states | grep -q "$1"; }
no_state()  { ! states | grep -q "$1"; }
setst() { "$TMP/setstate" "$win" "$@" >/dev/null 2>&1 || true; sleep 0.6; }
frame_geom() {
	f=$(get_parent_window "$win")
	[ -n "$f" ] || f=$(to_hex "$win")
	xwininfo -display "$DISPLAY" -id "$f" 2>/dev/null |
		awk '/Absolute upper-left X/ {x=$4} /Absolute upper-left Y/ {y=$4}
		     /^  Width:/ {w=$2} /^  Height:/ {h=$2} END {print w"x"h"+"x"+"y}'
}

# ------------------------------------------------------------------
# 1. ADD / REMOVE / TOGGLE
# ------------------------------------------------------------------
setst add _NET_WM_STATE_ABOVE
has_state _NET_WM_STATE_ABOVE || fail "(1) ADD で ABOVE が付きません: $(states)"

setst remove _NET_WM_STATE_ABOVE
no_state _NET_WM_STATE_ABOVE || fail "(1) REMOVE で ABOVE が消えません: $(states)"

setst toggle _NET_WM_STATE_ABOVE
has_state _NET_WM_STATE_ABOVE || fail "(1) TOGGLE で ABOVE が付きません: $(states)"
setst toggle _NET_WM_STATE_ABOVE
no_state _NET_WM_STATE_ABOVE || fail "(1) TOGGLE の 2 回目で ABOVE が消えません: $(states)"

# ------------------------------------------------------------------
# 2. 1 メッセージで 2 状態
# ------------------------------------------------------------------
setst add _NET_WM_STATE_ABOVE _NET_WM_STATE_STICKY
has_state _NET_WM_STATE_ABOVE  || fail "(2) 同時指定で ABOVE が付きません: $(states)"
has_state _NET_WM_STATE_STICKY || fail "(2) 同時指定で STICKY が付きません: $(states)"

setst remove _NET_WM_STATE_ABOVE _NET_WM_STATE_STICKY
no_state _NET_WM_STATE_ABOVE  || fail "(2) 同時解除で ABOVE が残ります: $(states)"
no_state _NET_WM_STATE_STICKY || fail "(2) 同時解除で STICKY が残ります: $(states)"

# ------------------------------------------------------------------
# 3. 未知アトムを混ぜても既知の方は処理される
# ------------------------------------------------------------------
setst add _NET_WM_STATE_ABOVE _W98WM_TEST_BOGUS_STATE
wm_alive || fail "(3) 未知アトムを混ぜたら WM が死にました"
has_state _NET_WM_STATE_ABOVE ||
	fail "(3) 未知アトムを混ぜると既知の状態も処理されません: $(states)"
no_state _W98WM_TEST_BOGUS_STATE ||
	fail "(3) 未知アトムがそのまま _NET_WM_STATE に載っています: $(states)"
setst remove _NET_WM_STATE_ABOVE

# ------------------------------------------------------------------
# 4. 最大化の軸ごとの組み合わせ
# ------------------------------------------------------------------
before=$(frame_geom)

setst add _NET_WM_STATE_MAXIMIZED_VERT
has_state MAXIMIZED_VERT || fail "(4) 縦のみ最大化ができません: $(states)"
no_state MAXIMIZED_HORZ  || fail "(4) 縦だけ要求したのに横も最大化されました: $(states)"
v_only=$(frame_geom)

setst add _NET_WM_STATE_MAXIMIZED_HORZ
has_state MAXIMIZED_VERT || fail "(4) 横を足したら縦が外れました: $(states)"
has_state MAXIMIZED_HORZ || fail "(4) 横を足せません: $(states)"
both=$(frame_geom)
[ "$both" != "$v_only" ] || fail "(4) 横を足してもジオメトリが変わりません ($both)"

# 片軸だけ解除 → もう片軸は保たれること
setst remove _NET_WM_STATE_MAXIMIZED_HORZ
has_state MAXIMIZED_VERT ||
	fail "(4) **横だけ解除したのに縦まで外れました**: $(states)"
no_state MAXIMIZED_HORZ || fail "(4) 横が解除されません: $(states)"
back_to_v=$(frame_geom)
[ "$back_to_v" = "$v_only" ] ||
	fail "(4) 横を解除しても縦のみの状態に戻りません ($v_only -> $back_to_v)"

setst remove _NET_WM_STATE_MAXIMIZED_VERT
no_state MAXIMIZED_VERT || fail "(4) 縦が解除されません: $(states)"
restored=$(frame_geom)
[ "$restored" = "$before" ] ||
	fail "(4) 全解除で元のジオメトリに戻りません ($before -> $restored)"

# 逆順（横 → 縦 → 縦だけ解除）
setst add _NET_WM_STATE_MAXIMIZED_HORZ
h_only=$(frame_geom)
setst add _NET_WM_STATE_MAXIMIZED_VERT
setst remove _NET_WM_STATE_MAXIMIZED_VERT
has_state MAXIMIZED_HORZ ||
	fail "(4) **縦だけ解除したのに横まで外れました**: $(states)"
[ "$(frame_geom)" = "$h_only" ] ||
	fail "(4) 縦を解除しても横のみの状態に戻りません"
setst remove _NET_WM_STATE_MAXIMIZED_HORZ

# ------------------------------------------------------------------
# 5. 最大化 → 移動 → 復元
# ------------------------------------------------------------------
xdotool windowmove "$win" 300 250 >/dev/null 2>&1 || true
sleep 0.8
moved=$(frame_geom)

setst add _NET_WM_STATE_MAXIMIZED_VERT _NET_WM_STATE_MAXIMIZED_HORZ
sleep 0.5
setst remove _NET_WM_STATE_MAXIMIZED_VERT _NET_WM_STATE_MAXIMIZED_HORZ
after_restore=$(frame_geom)
[ "$after_restore" = "$moved" ] ||
	fail "(5) 最大化→復元で移動後の位置に戻りません ($moved -> $after_restore)"

# ------------------------------------------------------------------
# 6. 高速連打
# ------------------------------------------------------------------
k=0
while [ $k -lt 30 ]; do
	"$TMP/setstate" "$win" toggle _NET_WM_STATE_ABOVE >/dev/null 2>&1 || true
	k=$((k + 1))
done
sleep 1.5
wm_alive || fail "(6) 高速連打で WM が死にました"
# 30 回 = 偶数回なので元に戻っているはず
no_state _NET_WM_STATE_ABOVE ||
	fail "(6) TOGGLE を偶数回送ったのに ABOVE が残っています: $(states)"

k=0
while [ $k -lt 20 ]; do
	"$TMP/setstate" "$win" toggle \
		_NET_WM_STATE_MAXIMIZED_VERT _NET_WM_STATE_MAXIMIZED_HORZ \
		>/dev/null 2>&1 || true
	k=$((k + 1))
done
sleep 2
wm_alive || fail "(6) 最大化の高速連打で WM が死にました"
setst remove _NET_WM_STATE_MAXIMIZED_VERT _NET_WM_STATE_MAXIMIZED_HORZ

# ------------------------------------------------------------------
# 7. SKIP_TASKBAR がタスクバーに反映される
# ------------------------------------------------------------------
panel=$(xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
	awk '/"w98wm taskbar"/ { print $1; exit }')
if [ -n "$panel" ] && command -v convert >/dev/null 2>&1; then
	# タスクバーの中身が変わったことを、パネルの画素の合計で見る
	sig_before=$(xwd -display "$DISPLAY" -id "$panel" 2>/dev/null |
		convert xwd:- -format '%[mean]' info: 2>/dev/null || echo "")
	setst add _NET_WM_STATE_SKIP_TASKBAR
	sleep 0.8
	sig_after=$(xwd -display "$DISPLAY" -id "$panel" 2>/dev/null |
		convert xwd:- -format '%[mean]' info: 2>/dev/null || echo "")

	if [ -n "$sig_before" ] && [ -n "$sig_after" ]; then
		[ "$sig_before" != "$sig_after" ] ||
			fail "(7) SKIP_TASKBAR を付けてもタスクバーの見た目が変わりません"
	fi
	has_state SKIP_TASKBAR || fail "(7) SKIP_TASKBAR が状態に載りません: $(states)"
	setst remove _NET_WM_STATE_SKIP_TASKBAR
else
	echo "# タスクバーか ImageMagick が無いので SKIP_TASKBAR の見た目確認は省略"
	setst add _NET_WM_STATE_SKIP_TASKBAR
	has_state SKIP_TASKBAR || fail "(7) SKIP_TASKBAR が状態に載りません: $(states)"
	setst remove _NET_WM_STATE_SKIP_TASKBAR
fi

# ------------------------------------------------------------------
# 8. apply_simple_state の対象を一通り
#
#    実装は MODAL / STICKY / SKIP_TASKBAR / SKIP_PAGER / ABOVE / BELOW /
#    DEMANDS_ATTENTION を同じ経路で扱う。1 つでも取りこぼしていないか
#    機械的に確認する（_NET_SUPPORTED に載せる以上、全部に責任がある）。
# ------------------------------------------------------------------
for st in _NET_WM_STATE_MODAL _NET_WM_STATE_SKIP_PAGER \
          _NET_WM_STATE_BELOW _NET_WM_STATE_DEMANDS_ATTENTION; do
	short=$(printf '%s' "$st" | sed 's/_NET_WM_STATE_//')
	setst add "$st"
	has_state "$short" || fail "(8) $st を ADD しても状態に載りません: $(states)"
	setst remove "$st"
	no_state "$short" || fail "(8) $st を REMOVE しても残ります: $(states)"
done

# ------------------------------------------------------------------
# 9. FULLSCREEN は専用経路 (layout_fullscreen)
# ------------------------------------------------------------------
geom_before_fs=$(frame_geom)
setst add _NET_WM_STATE_FULLSCREEN
has_state FULLSCREEN || fail "(9) 全画面になりません: $(states)"
fs_geom=$(frame_geom)
[ "$fs_geom" != "$geom_before_fs" ] || fail "(9) 全画面でジオメトリが変わりません"

# 全画面はモニタ全域（作業領域ではない）。タスクバーの分を引いていないこと
scr_w=$(xdpyinfo -display "$DISPLAY" 2>/dev/null |
	awk '/dimensions:/ {split($2,a,"x"); print a[1]; exit}')
scr_h=$(xdpyinfo -display "$DISPLAY" 2>/dev/null |
	awk '/dimensions:/ {split($2,a,"x"); print a[2]; exit}')
if [ -n "$scr_w" ] && [ -n "$scr_h" ]; then
	[ "$fs_geom" = "${scr_w}x${scr_h}+0+0" ] ||
		fail "(9) 全画面がモニタ全域になっていません: $fs_geom (期待 ${scr_w}x${scr_h}+0+0)"
fi

setst remove _NET_WM_STATE_FULLSCREEN
no_state FULLSCREEN || fail "(9) 全画面を解除できません: $(states)"
[ "$(frame_geom)" = "$geom_before_fs" ] ||
	fail "(9) 全画面の解除で元のジオメトリに戻りません"

# ------------------------------------------------------------------
# 10. HIDDEN / FOCUSED は **WM が設定するもの**
#
#     EWMH 上、この 2 つはクライアントが要求するものではない。
#     要求されても最小化したりフォーカスを奪ったりしないこと。
#     (_NET_SUPPORTED には載せる —— pager が読むため)
# ------------------------------------------------------------------
setst add _NET_WM_STATE_HIDDEN
wm_alive || fail "(10) HIDDEN の要求で WM が死にました"
st_now=$(xprop -display "$DISPLAY" -id "$win" WM_STATE 2>/dev/null |
	awk '/window state:/ { print $3; exit }')
[ "$st_now" = "Normal" ] ||
	fail "(10) クライアントの HIDDEN 要求で最小化されました (WM_STATE=$st_now)。HIDDEN は WM が設定するもの"

setst add _NET_WM_STATE_FOCUSED
wm_alive || fail "(10) FOCUSED の要求で WM が死にました"

wm_alive || fail "最終確認で WM が死んでいました"
echo "# _NET_WM_STATE の遷移 (add/remove/toggle / 同時指定 / 未知アトム / 軸別最大化 / 連打) を確認"
exit 0
