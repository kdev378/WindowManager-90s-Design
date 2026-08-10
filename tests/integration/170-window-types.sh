#!/bin/sh
#
# 170-window-types.sh
#
# `_NET_WM_WINDOW_TYPE` の **13 種すべて** について、SPEC §5.2.2 の表どおりに
# 装飾・キャプションの大きさ・レイヤ・フォーカス可否が決まることを検証する。
#
# なぜ要るか
# ----------
# 010 は「13 個のアトムが `_NET_SUPPORTED` に載っている」ことしか見ていない。
# しかし種別が実際に効くのは
#
#   _NET_WM_WINDOW_TYPE を読む → type_table[] を引く
#     → 装飾を付けるか / どのレイヤに置くか / フォーカスを渡すか / タスクバーに出すか
#
# の連鎖であって、`src/type.c` の表を **1 行間違えても** 010 は通ってしまう。
# しかも間違いは「ツールチップにタイトルバーが付く」「ドロップダウンが
# 親の下に潜る」といった、実アプリを 1 つ動かしただけでは出ない形で現れる。
#
# SPEC §5.2.2 の表はそのままテストの期待値表に変換できるので、
# ここでは表を **このスクリプトに再掲** して機械的に突き合わせる。
# src/type.c とこのファイルの両方を同時に間違えない限り、ずれは検出される。
#
# 期待値（SPEC §5.2.2 / src/type.c の表）:
#
#   種別              装飾            レイヤ    フォーカス タスクバー
#   DESKTOP           無              desktop   可         非表示
#   DOCK              無              dock      不可       非表示
#   TOOLBAR           小キャプション  normal    可         非表示
#   MENU              小キャプション  normal    可         非表示
#   UTILITY           小キャプション  normal    可         非表示
#   SPLASH            無              normal    不可       非表示
#   DIALOG            有              normal    可         表示
#   DROPDOWN_MENU     無              above     不可       非表示
#   POPUP_MENU        無              above     不可       非表示
#   TOOLTIP           無              above     不可       非表示
#   NOTIFICATION      無              above     不可       非表示
#   COMBO             無              above     不可       非表示
#   DND               無              above     不可       非表示
#   NORMAL            有              normal    可         表示
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"

# SPEC §4.2 のメトリクス (96dpi, scale=1)
BORDER=4
CAPTION=18
CAPTION_SMALL=13
FULL_TOP=$((BORDER + CAPTION))
SMALL_TOP=$((BORDER + CAPTION_SMALL))

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/typed.c" <<'EOF'
/*
 * 指定した _NET_WM_WINDOW_TYPE を持つ窓を 1 枚作る。
 *
 *   typed <型名の末尾>      例: typed TOOLTIP
 *
 * 型名は _NET_WM_WINDOW_TYPE_<末尾> に展開する。
 * 窓 ID を出してから居座る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t type_atom, wt_atom;
	char name[80];
	uint32_t vals[1];

	if (argc < 2)
		return 2;
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	snprintf(name, sizeof name, "_NET_WM_WINDOW_TYPE_%s", argv[1]);

	r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen("_NET_WM_WINDOW_TYPE"),
		                "_NET_WM_WINDOW_TYPE"), NULL);
	wt_atom = r ? r->atom : 0;
	free(r);

	r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	type_atom = r ? r->atom : 0;
	free(r);

	w = xcb_generate_id(c);
	vals[0] = 0xc0c0c0;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 50, 50, 220, 150, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL, vals);

	snprintf(name, sizeof name, "w98wm-type-%s", argv[1]);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(name), name);

	/* 型は map の前に立てる (WM は MapRequest の時点で読む) */
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		wt_atom, XCB_ATOM_ATOM, 32, 1, &type_atom);

	xcb_map_window(c, w);
	xcb_flush(c);
	printf("0x%x\n", w);
	fflush(stdout);

	for (;;)
		sleep(1);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/typed" "$TMP/typed.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

frame_extents() {
	xprop -display "$DISPLAY" -id "$1" _NET_FRAME_EXTENTS 2>/dev/null |
		sed 's/.*= *//' | tr -d ' '
}
active_window() {
	xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null | sed 's/.*# *//'
}
stacking() {
	xprop -display "$DISPLAY" -root _NET_CLIENT_LIST_STACKING 2>/dev/null |
		sed 's/.*# *//' | tr -d ' '
}
wm_alive() { kill -0 "$WM_PID" 2>/dev/null; }

# 種別ごとの期待値（SPEC §5.2.2）。deco: none|full|small
#                  種別            deco   layer    focus
TYPES="DESKTOP:none:desktop:yes
DOCK:none:dock:no
TOOLBAR:small:normal:yes
MENU:small:normal:yes
UTILITY:small:normal:yes
SPLASH:none:normal:no
DIALOG:full:normal:yes
DROPDOWN_MENU:none:above:no
POPUP_MENU:none:above:no
TOOLTIP:none:above:no
NOTIFICATION:none:above:no
COMBO:none:above:no
DND:none:above:no
NORMAL:full:normal:yes"

# レイヤ名 → 順序（小さいほど下。SPEC §3.7.1）
layer_rank() {
	case "$1" in
	desktop) echo 0 ;;
	below)   echo 1 ;;
	normal)  echo 2 ;;
	dock)    echo 3 ;;
	above)   echo 4 ;;
	*)       echo 9 ;;
	esac
}

order_file="$TMP/order.txt"
: >"$order_file"
fails=0

echo "$TYPES" | while IFS=: read -r ty deco layer focus; do
	[ -n "$ty" ] || continue

	# 直前のアクティブウィンドウを控えてから開く
	prev_active=$(active_window)

	"$TMP/typed" "$ty" >"$TMP/w_$ty.txt" 2>"$TMP/e_$ty.txt" &
	p=$!
	echo "$p" >>"$TMP/pids.txt"

	win=""
	i=0
	while [ $i -lt 50 ]; do
		win=$(head -n1 "$TMP/w_$ty.txt" 2>/dev/null)
		[ -n "$win" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	if [ -z "$win" ]; then
		echo "FAILTYPE $ty 窓を作れませんでした" >>"$TMP/fails.txt"
		continue
	fi
	sleep 1.2

	printf '%s %s %s %s %s\n' "$ty" "$win" "$deco" "$layer" "$focus" >>"$order_file"

	# ---- 装飾 ----
	fe=$(frame_extents "$win")
	case "$deco" in
	none)
		[ "$fe" = "0,0,0,0" ] ||
			echo "FAILTYPE $ty 装飾なしのはずが _NET_FRAME_EXTENTS=$fe" >>"$TMP/fails.txt"
		;;
	full)
		[ "$fe" = "$BORDER,$BORDER,$FULL_TOP,$BORDER" ] ||
			echo "FAILTYPE $ty 通常キャプションのはずが _NET_FRAME_EXTENTS=$fe (期待 $BORDER,$BORDER,$FULL_TOP,$BORDER)" >>"$TMP/fails.txt"
		;;
	small)
		[ "$fe" = "$BORDER,$BORDER,$SMALL_TOP,$BORDER" ] ||
			echo "FAILTYPE $ty 小キャプションのはずが _NET_FRAME_EXTENTS=$fe (期待 $BORDER,$BORDER,$SMALL_TOP,$BORDER)" >>"$TMP/fails.txt"
		;;
	esac

	# ---- フォーカス ----
	now_active=$(active_window)
	short=$(printf '%x' "$win")
	case "$focus" in
	yes)
		case "$now_active" in
		*"$short"*) ;;
		*) echo "FAILTYPE $ty フォーカスを取るはずが _NET_ACTIVE_WINDOW=$now_active" >>"$TMP/fails.txt" ;;
		esac
		;;
	no)
		case "$now_active" in
		*"$short"*)
			echo "FAILTYPE $ty フォーカスを取らないはずが _NET_ACTIVE_WINDOW になりました" >>"$TMP/fails.txt"
			;;
		esac
		;;
	esac
done

wm_alive || fail "13 種を開く途中で WM が死にました (ログ: $WM_PID_LOG)"

if [ -s "$TMP/fails.txt" ]; then
	echo "# --- SPEC §5.2.2 の表と食い違った種別 ---"
	sed 's/^FAILTYPE /#   /' "$TMP/fails.txt"
	fail "$(grep -c FAILTYPE "$TMP/fails.txt") 件の種別が SPEC §5.2.2 の表どおりになっていません"
fi

# ------------------------------------------------------------------
# レイヤの検証: _NET_CLIENT_LIST_STACKING の並び（下→上）が
# レイヤの順序と矛盾しないこと
# ------------------------------------------------------------------
stack=$(stacking)
[ -n "$stack" ] || fail "_NET_CLIENT_LIST_STACKING が読めません"

# stacking 中の位置を求めて、レイヤ順と単調であることを確認する
pos_of() {
	printf '%s' "$stack" | tr ',' '\n' | grep -n -i -x "$1" 2>/dev/null |
		head -n1 | cut -d: -f1
}

#
# ★ 「レイヤが上がるときだけ隣と比べる」では検出できない。
#   期待レイヤを**下げる**方向に間違えた場合（例: DROPDOWN_MENU を
#   above ではなく desktop と書いた場合）、比較する条件に一度も入らず
#   素通りする。実際にそれで通ってしまった。
#
#   正しくは **レイヤをまたぐ全ペア**を見る。
#   レイヤごとに実際のスタック位置の最小と最大を集計し、
#   「下のレイヤの最大位置 < 上のレイヤの最小位置」が
#   隣接するレイヤすべてで成り立つことを確認する。
#   同一レイヤ内の順序は問わない（SPEC は規定していない）。
#
ranges="$TMP/ranges.txt"
: >"$ranges"
for rank in 0 1 2 3 4; do
	rmin=""
	rmax=""
	while read -r ty win deco layer focus; do
		[ -n "$ty" ] || continue
		[ "$(layer_rank "$layer")" = "$rank" ] || continue
		p=$(pos_of "$win")
		[ -n "$p" ] || continue
		if [ -z "$rmin" ] || [ "$p" -lt "$rmin" ]; then rmin=$p; fi
		if [ -z "$rmax" ] || [ "$p" -gt "$rmax" ]; then rmax=$p; fi
	done <"$order_file"
	[ -n "$rmin" ] && printf '%s %s %s\n' "$rank" "$rmin" "$rmax" >>"$ranges"
done

[ -s "$ranges" ] || fail "スタック位置を 1 つも解決できませんでした"

prev_rank=""
prev_max=""
viol=0
while read -r rank rmin rmax; do
	if [ -n "$prev_rank" ] && [ "$prev_max" -ge "$rmin" ]; then
		echo "#   レイヤ $prev_rank の最上位置 $prev_max が レイヤ $rank の最下位置 $rmin 以上です"
		viol=$((viol + 1))
	fi
	prev_rank=$rank
	prev_max=$rmax
done <"$ranges"

if [ "$viol" -ne 0 ]; then
	echo "# --- レイヤごとのスタック位置 (rank 最小 最大) ---"
	sed 's/^/#   /' "$ranges"
	echo "# --- 実際のスタック順 (下→上) ---"
	printf '%s' "$stack" | tr ',' '\n' | sed 's/^/#   /'
	echo "# --- 期待 (種別 窓 装飾 レイヤ フォーカス) ---"
	sed 's/^/#   /' "$order_file"
	fail "スタック順がレイヤの順序と矛盾しています ($viol 件)"
fi

# ------------------------------------------------------------------
# DESKTOP が最下、above 系が最上であることを直接確認
# ------------------------------------------------------------------
desk_win=$(awk '$1=="DESKTOP" {print $2}' "$order_file")
tip_win=$(awk '$1=="TOOLTIP" {print $2}' "$order_file")
norm_win=$(awk '$1=="NORMAL" {print $2}' "$order_file")

if [ -n "$desk_win" ] && [ -n "$norm_win" ]; then
	dp=$(pos_of "$desk_win"); np=$(pos_of "$norm_win")
	if [ -n "$dp" ] && [ -n "$np" ]; then
		[ "$dp" -lt "$np" ] ||
			fail "DESKTOP が NORMAL より上にあります (desktop=$dp normal=$np)"
	fi
fi
if [ -n "$tip_win" ] && [ -n "$norm_win" ]; then
	tp=$(pos_of "$tip_win"); np=$(pos_of "$norm_win")
	if [ -n "$tp" ] && [ -n "$np" ]; then
		[ "$tp" -gt "$np" ] ||
			fail "TOOLTIP (above) が NORMAL より下にあります (tooltip=$tp normal=$np)"
	fi
fi

# ------------------------------------------------------------------
# タスクバー掲載: 代表 2 組で見る
#
#   NORMAL / DIALOG → 出る
#   TOOLTIP / DOCK  → 出ない
#
# 内部状態を直接は読めないので、パネルの画素が変わるかで見る。
# ------------------------------------------------------------------
panel=$(xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
	awk '/"w98wm taskbar"/ { print $1; exit }')
if [ -n "$panel" ] && command -v convert >/dev/null 2>&1 && command -v xwd >/dev/null 2>&1; then
	panel_sig() {
		xwd -display "$DISPLAY" -id "$panel" 2>/dev/null |
			convert xwd:- -format '%[mean]' info: 2>/dev/null || echo ""
	}

	sig0=$(panel_sig)

	# タスクバーに出ない型を足しても見た目は変わらないはず
	"$TMP/typed" TOOLTIP >"$TMP/w_tip2.txt" 2>/dev/null &
	echo "$!" >>"$TMP/pids.txt"
	sleep 1.5
	sig_tip=$(panel_sig)

	# タスクバーに出る型を足すと変わるはず
	"$TMP/typed" NORMAL >"$TMP/w_norm2.txt" 2>/dev/null &
	echo "$!" >>"$TMP/pids.txt"
	sleep 1.5
	sig_norm=$(panel_sig)

	if [ -n "$sig0" ] && [ -n "$sig_tip" ] && [ -n "$sig_norm" ]; then
		[ "$sig0" = "$sig_tip" ] ||
			fail "TOOLTIP を開いたのにタスクバーの見た目が変わりました (非表示のはず)"
		[ "$sig_tip" != "$sig_norm" ] ||
			fail "NORMAL を開いてもタスクバーの見た目が変わりません (表示のはず)"
	fi
else
	echo "# タスクバーか ImageMagick が無いので掲載の確認は省略"
fi

# 起動したクライアントを片付ける
if [ -f "$TMP/pids.txt" ]; then
	while read -r p; do
		kill "$p" 2>/dev/null || true
	done <"$TMP/pids.txt"
fi

wm_alive || fail "最終確認で WM が死んでいました"
echo "# ウィンドウ種別 14 種 (13 + NORMAL) の装飾・キャプション・レイヤ・フォーカスを SPEC §5.2.2 の表と突き合わせ"
exit 0
