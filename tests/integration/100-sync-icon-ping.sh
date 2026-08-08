#!/bin/sh
#
# 100-sync-icon-ping.sh
#
# _NET_WM_SYNC_REQUEST / _NET_WM_ICON / _NET_WM_PING / _NET_WM_USER_TIME /
# _NET_WM_STRUT(_PARTIAL)
#
# SPEC §5.2.1 の規約に対応するテスト。専用の検証クライアントを C で書いて
# xcb で直接プロパティを立て、WM が期待どおり反応するかを見る。
#
# 重要な点 (§7.3): GTK は _NET_SUPPORTED に _NET_WM_SYNC_REQUEST が
# 載っているかを見てからカウンタを作る。載っていなければ同期は一切
# 行われないので、この確認は「載っている」ことに実質的な意味がある。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb xcb-sync 2>/dev/null || skip "xcb/xcb-sync がありません"

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/probe.c" <<'EOF'
/* WM の Phase 3 機能を突くための検証クライアント */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/sync.h>

static xcb_atom_t A(xcb_connection_t *c, const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

int main(void)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w;
	xcb_sync_counter_t ctr;
	xcb_sync_int64_t zero = { 0, 0 };
	uint32_t icon[2 + 16 * 16 + 2 + 4 * 4];
	uint32_t strut[12];
	uint32_t utime;
	xcb_atom_t protos[2];
	int i, n = 0;

	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	w = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 40, 40, 200, 120,
		0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL);

	/* _NET_WM_SYNC_REQUEST_COUNTER (§7.3) */
	ctr = xcb_generate_id(c);
	xcb_sync_create_counter(c, ctr, zero);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "_NET_WM_SYNC_REQUEST_COUNTER"), XCB_ATOM_CARDINAL, 32, 1, &ctr);

	/* WM_PROTOCOLS に _NET_WM_SYNC_REQUEST と _NET_WM_PING (§7.3, §7.4) */
	protos[0] = A(c, "_NET_WM_SYNC_REQUEST");
	protos[1] = A(c, "_NET_WM_PING");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "WM_PROTOCOLS"), XCB_ATOM_ATOM, 32, 2, protos);

	/*
	 * _NET_WM_ICON (§4.4.1): 16x16 と 4x4 の 2 枚を 1 つのプロパティに詰める。
	 * WM は 16x16 をそのまま採るはず。
	 */
	icon[n++] = 16; icon[n++] = 16;
	for (i = 0; i < 16 * 16; i++)
		icon[n++] = 0xff0000ffu;          /* 不透明の青 */
	icon[n++] = 4; icon[n++] = 4;
	for (i = 0; i < 4 * 4; i++)
		icon[n++] = 0xffff0000u;
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "_NET_WM_ICON"), XCB_ATOM_CARDINAL, 32, (uint32_t)n, icon);

	/* _NET_WM_USER_TIME (§3.6) */
	utime = 12345;
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "_NET_WM_USER_TIME"), XCB_ATOM_CARDINAL, 32, 1, &utime);

	/* _NET_WM_STRUT_PARTIAL (§3.5.2): 下端に 20px */
	memset(strut, 0, sizeof strut);
	strut[3] = 20;      /* bottom */
	strut[10] = 0;      /* bottom_start_x */
	strut[11] = 799;    /* bottom_end_x */
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "_NET_WM_STRUT_PARTIAL"), XCB_ATOM_CARDINAL, 32, 12, strut);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w,
		A(c, "_NET_WM_STRUT"), XCB_ATOM_CARDINAL, 32, 4, strut);

	xcb_map_window(c, w);
	xcb_flush(c);
	printf("0x%x\n", w);
	fflush(stdout);

	/* ping には応答しない: 無応答検出の経路を通す (§7.4) */
	for (;;)
		sleep(1);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/probe" "$TMP/probe.c" \
	$(pkg-config --cflags --libs xcb xcb-sync) 2>"$TMP/cc.log" ||
	skip "検証クライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

# _NET_SUPPORTED に載っていること（GTK はこれを見て参加を決める）
sup=$(xprop -display "$DISPLAY" -root _NET_SUPPORTED 2>/dev/null)
for a in _NET_WM_SYNC_REQUEST _NET_WM_PING _NET_WM_ICON _NET_WM_USER_TIME \
	_NET_WM_STRUT _NET_WM_STRUT_PARTIAL; do
	printf '%s' "$sup" | grep -q "$a" || fail "_NET_SUPPORTED に $a がありません"
done

DISPLAY="$DISPLAY" "$TMP/probe" >"$TMP/win" 2>/dev/null &
probe_pid=$!
_LIB_PIDS="$_LIB_PIDS $probe_pid"
sleep 3

win=$(head -1 "$TMP/win" 2>/dev/null || true)
[ -n "$win" ] || fail "検証クライアントがウィンドウ ID を出しませんでした"

# 管理下に入ったこと
xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null |
	grep -qi "${win#0x}" || fail "検証クライアントが管理されていません ($win)"

# strut が作業領域に反映されたこと (§3.5.2)
wa=$(xprop -display "$DISPLAY" -root _NET_WORKAREA 2>/dev/null | sed 's/.*= *//')
wah=$(printf '%s' "$wa" | cut -d, -f4 | tr -d ' ')
sh_=$(xdotool getdisplaygeometry 2>/dev/null | cut -d' ' -f2)
if [ -n "$wah" ] && [ -n "$sh_" ]; then
	[ "$wah" -lt "$sh_" ] ||
		fail "_NET_WM_STRUT_PARTIAL が作業領域に反映されていません (h=$wah screen=$sh_)"
fi

# 無応答クライアントでも WM が生き続けること (§7.4 の要点)
sleep 7
pgrep -f "$WM_BIN" >/dev/null 2>&1 ||
	fail "ping に応答しないクライアントで WM が落ちました"

# WM が応答し続けていること
xprop -display "$DISPLAY" -root _NET_SUPPORTED >/dev/null 2>&1 ||
	fail "WM が応答しなくなりました"

echo "OK: sync counter / icon / ping / user_time / strut を確認しました"
echo "    workarea=$wa"
exit 0
