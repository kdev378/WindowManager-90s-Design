#!/bin/sh
#
# 150-icccm-abuse.sh
#
# ICCCM プロパティの **異常系**（SPEC §3.2, §3.4, §3.7.2, §5.1）。
#
# なぜ専用クライアントが要るか
# ----------------------------
# 実アプリは（当然ながら）正しいヒントを立てる。だから Firefox や GTK を
# いくら起動しても、壊れた入力に対する経路は一度も通らない。
# しかし SPEC §3.7.2 は「transient のループは**悪意ある入力でも起こる**」と
# 明記しており、これは実アプリでは再現できない。
#
# ここでは意図的に壊れたプロパティを立てるクライアントを生成し、
# **WM が落ちない・固まらない・ウィンドウを失わない**ことを確認する。
# 「正しく解釈すること」までは求めない（何が正しいかが定義できない入力なので）。
# 求めるのは「壊れた入力で WM が死なないこと」。
#
# 検証項目:
#   1. WM_NORMAL_HINTS が壊れている (min>max / 0 / inc=0 / gravity 未知)
#   2. WM_TRANSIENT_FOR が自己参照
#   3. transient のループ (A→B→A)
#   4. transient の鎖が WM_TRANSIENT_DEPTH(16) を超える
#   5. WM_PROTOCOLS が無い / WM_DELETE_WINDOW が無い窓を閉じる
#   6. WM_HINTS の異常値 (未知 flags / input が範囲外)
#   7. WM_STATE を**クライアントが勝手に**不正値で書く
#   8. 上記すべてのあと WM が生存し、リサイズ・最大化・閉じるが機能する
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/abuse.c" <<'EOF'
/*
 * 壊れた ICCCM プロパティを立てる検証クライアント。
 *
 *   abuse hints     … WM_NORMAL_HINTS を壊した窓を 4 枚
 *   abuse selfref   … WM_TRANSIENT_FOR が自分自身
 *   abuse loop      … A の transient_for = B, B の transient_for = A
 *   abuse chain N   … N 段の transient 鎖
 *   abuse noproto   … WM_PROTOCOLS を一切立てない
 *   abuse badhints  … WM_HINTS に未知 flags と範囲外の値
 *   abuse badstate  … WM_STATE をクライアントが不正値で書く
 *
 * 作った窓の ID を 1 行ずつ標準出力に出してから、終了せずに居座る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_screen_t *s;

static xcb_atom_t A(const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static xcb_window_t mkwin(const char *name, int16_t x, int16_t y)
{
	xcb_window_t w = xcb_generate_id(c);
	uint32_t vals[1];

	vals[0] = 0xc0c0c0;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, x, y, 200, 140, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL, vals);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(name), name);
	return w;
}

/* ICCCM の WM_SIZE_HINTS は 18 個の CARD32 */
static void set_size_hints(xcb_window_t w, const uint32_t h[18])
{
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
}

static void emit(xcb_window_t w)
{
	printf("0x%x\n", w);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "hints";

	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	if (strcmp(mode, "hints") == 0) {
		uint32_t h[18];
		xcb_window_t w;

		/* (a) min > max */
		memset(h, 0, sizeof h);
		h[0] = (1u << 4) | (1u << 5);     /* PMinSize | PMaxSize */
		h[5] = 800; h[6] = 600;           /* min 800x600 */
		h[7] = 100; h[8] = 80;            /* max 100x80  ← min より小さい */
		w = mkwin("abuse-min-gt-max", 20, 20);
		set_size_hints(w, h);
		xcb_map_window(c, w); emit(w);

		/* (b) 幅・高さ 0 */
		memset(h, 0, sizeof h);
		h[0] = (1u << 4) | (1u << 5) | (1u << 8);   /* Min|Max|Base */
		h[5] = 0; h[6] = 0;
		h[7] = 0; h[8] = 0;
		h[15] = 0; h[16] = 0;
		w = mkwin("abuse-zero-size", 240, 20);
		set_size_hints(w, h);
		xcb_map_window(c, w); emit(w);

		/* (c) resize increment = 0 (ゼロ除算を誘う) */
		memset(h, 0, sizeof h);
		h[0] = (1u << 6);                 /* PResizeInc */
		h[9] = 0; h[10] = 0;
		w = mkwin("abuse-zero-inc", 460, 20);
		set_size_hints(w, h);
		xcb_map_window(c, w); emit(w);

		/* (d) gravity が未知値 + アスペクト比の分母 0 */
		memset(h, 0, sizeof h);
		h[0] = (1u << 7) | (1u << 9);     /* PAspect | PWinGravity */
		h[11] = 16; h[12] = 0;            /* min aspect 16/0 */
		h[13] = 0;  h[14] = 0;            /* max aspect 0/0  */
		h[17] = 9999;                     /* gravity: 未定義 */
		w = mkwin("abuse-bad-gravity", 680, 20);
		set_size_hints(w, h);
		xcb_map_window(c, w); emit(w);

	} else if (strcmp(mode, "selfref") == 0) {
		xcb_window_t w = mkwin("abuse-selfref", 40, 200);
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
			XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &w);
		xcb_map_window(c, w); emit(w);

	} else if (strcmp(mode, "loop") == 0) {
		xcb_window_t a = mkwin("abuse-loop-a", 80, 240);
		xcb_window_t b = mkwin("abuse-loop-b", 320, 240);

		xcb_change_property(c, XCB_PROP_MODE_REPLACE, a,
			XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &b);
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, b,
			XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &a);
		xcb_map_window(c, a);
		xcb_map_window(c, b);
		emit(a); emit(b);

	} else if (strcmp(mode, "chain") == 0) {
		int n = argc > 2 ? atoi(argv[2]) : 20;
		xcb_window_t prev = XCB_WINDOW_NONE;
		int i;

		if (n < 2) n = 2;
		if (n > 64) n = 64;
		for (i = 0; i < n; i++) {
			char nm[48];
			xcb_window_t w;

			snprintf(nm, sizeof nm, "abuse-chain-%d", i);
			w = mkwin(nm, (int16_t)(10 + i * 5), (int16_t)(300 + i * 5));
			if (prev != XCB_WINDOW_NONE)
				xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
					XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &prev);
			xcb_map_window(c, w);
			emit(w);
			prev = w;
		}

	} else if (strcmp(mode, "noproto") == 0) {
		/* WM_PROTOCOLS を一切立てない = WM_DELETE_WINDOW を持たない */
		xcb_window_t w = mkwin("abuse-noproto", 400, 200);
		xcb_map_window(c, w); emit(w);

	} else if (strcmp(mode, "badhints") == 0) {
		/* WM_HINTS は 9 個の CARD32。未知 flags と範囲外の値を入れる */
		uint32_t h[9];
		xcb_window_t w;

		memset(h, 0, sizeof h);
		h[0] = 0xffffffffu;               /* flags: 全ビット立てる */
		h[1] = 0x7fffffffu;               /* input: 0/1 以外 */
		h[2] = 12345;                     /* initial_state: 未定義 */
		h[3] = 0xdeadbeefu;               /* icon_pixmap: 存在しない ID */
		h[4] = 0xdeadbeefu;               /* icon_window */
		h[7] = 0xdeadbeefu;               /* icon_mask */
		h[8] = 0xdeadbeefu;               /* window_group */
		w = mkwin("abuse-badhints", 500, 400);
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
			XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, h);
		xcb_map_window(c, w); emit(w);

	} else if (strcmp(mode, "badstate") == 0) {
		/* WM_STATE は本来 WM だけが書く。クライアントが不正値で書いてみる */
		uint32_t st[2];
		xcb_atom_t wm_state = A("WM_STATE");
		xcb_window_t w = mkwin("abuse-badstate", 620, 400);

		st[0] = 0xffffffffu;              /* state: 未定義 */
		st[1] = 0xdeadbeefu;              /* icon window: 存在しない */
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
			wm_state, wm_state, 32, 2, st);
		xcb_map_window(c, w); emit(w);
	}

	xcb_flush(c);
	for (;;)
		sleep(1);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/abuse" "$TMP/abuse.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

wm_alive() { kill -0 "$WM_PID" 2>/dev/null; }
n_clients() {
	xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
		tr ',' '\n' | grep -c '0x' || true
}

run_abuse() {   # run_abuse <モード> [引数] : 起動して窓 ID を並べて返す
	mode="$1"; shift
	out="$TMP/out_$mode.txt"
	"$TMP/abuse" "$mode" "$@" >"$out" 2>"$TMP/err_$mode.txt" &
	p=$!
	_LIB_PIDS="$_LIB_PIDS $p"
	i=0
	while [ $i -lt 50 ]; do
		[ -s "$out" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	sleep 1.5
	cat "$out"
}

before=$(n_clients)

# ------------------------------------------------------------------
# 1〜7. 壊れた入力を順に浴びせる。各段で WM が生きていることを見る
# ------------------------------------------------------------------
for mode in hints selfref loop noproto badhints badstate; do
	wins=$(run_abuse "$mode")
	wm_alive || fail "'$mode' で WM が死にました (ログ: $WM_PID_LOG)"
	[ -n "$wins" ] || fail "'$mode' で窓が 1 つも作られませんでした"
done

# transient の鎖は WM_TRANSIENT_DEPTH(16) を超える長さで試す
chain=$(run_abuse chain 24)
wm_alive || fail "24 段の transient 鎖で WM が死にました (ログ: $WM_PID_LOG)"

after=$(n_clients)
[ "${after:-0}" -gt "${before:-0}" ] ||
	fail "壊れたクライアントが 1 つも管理下に入りませんでした ($before -> $after)"

# ------------------------------------------------------------------
# 8. この状態で WM がまだ普通に働くか
#
#    「死んでいない」だけでは足りない。内部状態が壊れて
#    以後の操作を受け付けなくなっていないことまで見る。
# ------------------------------------------------------------------
require_tool xterm
xterm -display "$DISPLAY" -geometry 40x12+100+100 -T w98wm-abuse-sanity >/dev/null 2>&1 &
XT=$!
_LIB_PIDS="$_LIB_PIDS $XT"
# 名前ではなく PID で待つ (xterm の WM_NAME は中のシェルに上書きされる)
sane=$(wait_for_managed_window_by_pid "$XT" 20) ||
	fail "壊れた窓を浴びたあと、普通の窓が管理下に入りません"
sleep 1

# フォーカス
xdotool windowactivate --sync "$sane" >/dev/null 2>&1 || true
sleep 1
act=$(xprop -display "$DISPLAY" -root _NET_ACTIVE_WINDOW 2>/dev/null | sed 's/.*# *//')
case "$act" in
*"$(printf '%x' "$sane")"*) ;;
*) fail "壊れた窓を浴びたあと、フォーカスが機能しません (active=$act)" ;;
esac

# 最大化
if command -v wmctrl >/dev/null 2>&1; then
	wmctrl -i -r "$(to_hex "$sane")" -b add,maximized_vert,maximized_horz >/dev/null 2>&1 || true
	sleep 1
	wm_alive || fail "最大化で WM が死にました"
	wmctrl -i -r "$(to_hex "$sane")" -b remove,maximized_vert,maximized_horz >/dev/null 2>&1 || true
	sleep 0.5
	wm_alive || fail "最大化の解除で WM が死にました"
fi

# 閉じる
if command -v wmctrl >/dev/null 2>&1; then
	wmctrl -i -c "$(to_hex "$sane")" >/dev/null 2>&1 || true
	sleep 1.5
	wm_alive || fail "閉じる操作で WM が死にました"
fi

# ------------------------------------------------------------------
# 9. WM_DELETE_WINDOW を持たない窓を閉じさせる
#
#    ICCCM 上、この窓は XKillClient で殺すしかない。
#    WM がそれを避けて何もしないか、殺すか、どちらでもよいが
#    **WM 自身が巻き添えで死んではならない**。
# ------------------------------------------------------------------
noproto=$(head -n1 "$TMP/out_noproto.txt" 2>/dev/null || true)
if [ -n "$noproto" ] && command -v wmctrl >/dev/null 2>&1; then
	wmctrl -i -c "$noproto" >/dev/null 2>&1 || true
	sleep 1.5
	wm_alive || fail "WM_DELETE_WINDOW を持たない窓を閉じたら WM が死にました"
fi

# ------------------------------------------------------------------
# 10. X エラーがログに溜まっていないか
#
#     §2.2.2 は「自リソース宛のエラーは実装バグとして報告する」と定めている。
#     壊れた**クライアント**のプロパティを読んだ結果として
#     自リソース宛のエラーが出ることは、本来ありえない。
# ------------------------------------------------------------------
if grep -q "自リソースへのエラー" "$WM_PID_LOG" 2>/dev/null; then
	echo "# --- WM のログ ---"
	grep "自リソースへのエラー" "$WM_PID_LOG" | sed 's/^/#   /' | head -10
	fail "壊れた入力の処理中に自リソース宛の X エラーが出ました (§2.2.2)"
fi

wm_alive || fail "最終確認で WM が死んでいました"
echo "# ICCCM 異常系 (壊れたヒント / 自己参照 / ループ / 24 段の鎖 / 異常な WM_HINTS・WM_STATE) を確認"
exit 0
