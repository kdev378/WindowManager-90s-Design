#!/bin/sh
#
# 120-tray-xembed.sh
#
# システムトレイの埋め込み経路（SPEC §4.8、XEmbed / freedesktop System Tray
# Protocol）の検証。
#
# 110 はセレクションを取れたことしか見ていない。埋め込みの本体
# —— REQUEST_DOCK を受けて reparent し、_XEMBED_INFO の mapped フラグに
# 従って map/unmap し、アイコンが消えたら並べ直す —— は 110 では
# 一切通っていない。ここを実アプリ (Discord 等) 待ちにすると
# CI では永久に未検証のままになるので、100 と同じく検証用の
# トレイクライアントを C で書いて突く。
#
# 検証項目:
#   1. MANAGER 告知を受け取れる（クライアントは通常これを待って dock する）
#   2. REQUEST_DOCK したアイコンがトレイの子に reparent される
#   3. _XEMBED_INFO の XEMBED_MAPPED が立っていれば map される
#   4. アイコンが増えるとトレイの幅が広がる（タスクバーの配置に反映される）
#   5. _XEMBED_INFO を落とすと unmap される
#   6. アイコンを破棄するとトレイの幅が戻る（後始末の確認）
#   7. アイコンは WM の正常終了後も**破棄されない**（クライアントの資源。§4.8）
#   8. **WM を kill -9 で落としてもアイコンが残る**（save-set。§3.1.1）
#      —— トレイのコンテナは WM の資源なので、DestroyWindow は所有者に
#      関係なく子を道連れにする。save-set に入れ忘れると
#      「WM が落ちると常駐アプリのアイコンが二度と戻らない」形で壊れる。
#      実際に入れ忘れていて、このテストで見つかった。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

command -v cc >/dev/null 2>&1 || skip "cc がありません"
pkg-config --exists xcb 2>/dev/null || skip "xcb がありません"
require_tool xwininfo

TMP=$(mktemp -d)
cleanup_tmp() { rm -rf "$TMP"; }
trap 'cleanup_tmp' EXIT

cat >"$TMP/trayclient.c" <<'EOF'
/*
 * 検証用のトレイクライアント。
 *
 * freedesktop の System Tray Protocol に従って
 *   1. _NET_SYSTEM_TRAY_S<n> の所有者を探す
 *   2. 見つからなければ MANAGER クライアントメッセージを待つ
 *   3. _XEMBED_INFO を立ててから SYSTEM_TRAY_REQUEST_DOCK を送る
 * を行う。標準入力に 'u' が来たら _XEMBED_INFO の MAPPED を落とし、
 * 'm' で戻し、'q' で終了する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

#define XEMBED_MAPPED            (1u << 0)
#define SYSTEM_TRAY_REQUEST_DOCK 0

static xcb_atom_t A(xcb_connection_t *c, const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static void set_xembed_info(xcb_connection_t *c, xcb_window_t w,
                            xcb_atom_t info, int mapped)
{
	uint32_t v[2];
	v[0] = 0;                                   /* version */
	v[1] = mapped ? XEMBED_MAPPED : 0;          /* flags   */
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, info, info, 32, 2, v);
	xcb_flush(c);
}

int main(int argc, char **argv)
{
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_screen_t *s;
	xcb_window_t w, owner = XCB_WINDOW_NONE;
	xcb_atom_t sel, opcode, info, manager;
	xcb_get_selection_owner_reply_t *sr;
	xcb_client_message_event_t ev;
	char name[48];
	uint32_t vals[1];
	int tries;
	int ch;

	(void)argc; (void)argv;
	if (xcb_connection_has_error(c))
		return 1;
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", 0);
	sel     = A(c, name);
	opcode  = A(c, "_NET_SYSTEM_TRAY_OPCODE");
	info    = A(c, "_XEMBED_INFO");
	manager = A(c, "MANAGER");
	(void)manager;

	/* MANAGER 告知は起動順によっては取り逃すので、所有者を直接引く */
	for (tries = 0; tries < 50; tries++) {
		sr = xcb_get_selection_owner_reply(c,
			xcb_get_selection_owner(c, sel), NULL);
		if (sr != NULL && sr->owner != XCB_WINDOW_NONE) {
			owner = sr->owner;
			free(sr);
			break;
		}
		free(sr);
		usleep(200000);
	}
	if (owner == XCB_WINDOW_NONE) {
		fprintf(stderr, "no tray owner\n");
		return 2;
	}

	w = xcb_generate_id(c);
	vals[0] = 0x00ff00;                 /* 埋め込み後に見える色 */
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 16, 16, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL, vals);

	/* dock 要求の**前**に _XEMBED_INFO を立てるのがプロトコルの順序 */
	set_xembed_info(c, w, info, 1);

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = s->root;
	ev.format = 32;
	ev.type = opcode;
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = SYSTEM_TRAY_REQUEST_DOCK;
	ev.data.data32[2] = w;
	xcb_send_event(c, 0, owner, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(c);

	printf("0x%x\n", w);
	fflush(stdout);

	while ((ch = getchar()) != EOF) {
		if (ch == 'u')
			set_xembed_info(c, w, info, 0);
		else if (ch == 'm')
			set_xembed_info(c, w, info, 1);
		else if (ch == 'd') {
			xcb_destroy_window(c, w);
			xcb_flush(c);
		} else if (ch == 'q')
			break;
	}

	/* 明示的に切断しない: WM 側の後始末 (reparent back) を観察するため */
	for (;;)
		sleep(1);
	return 0;
}
EOF

cc -std=c99 -o "$TMP/trayclient" "$TMP/trayclient.c" \
	$(pkg-config --cflags --libs xcb) 2>"$TMP/cc.log" ||
	skip "検証用トレイクライアントをビルドできません: $(head -3 "$TMP/cc.log")"

start_xvfb
start_wm

# ------------------------------------------------------------------
# トレイのコンテナを待つ
# ------------------------------------------------------------------
find_tray() {
	xwininfo -display "$DISPLAY" -root -children 2>/dev/null |
		awk '/"w98wm tray"/ { print $1; exit }'
}
tray=""
i=0
while [ $i -lt 40 ]; do
	tray=$(find_tray)
	[ -n "$tray" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$tray" ] || fail "トレイのコンテナが見つかりません"

# xwininfo の子の個数の行は 3 通りある。両方とも実際に踏んだので列挙しておく:
#   "0 children."   (0 個。**コロンではなくピリオド**)
#   "1 child:"      (1 個。**単数形**)
#   "3 children:"   (2 個以上)
# /children:/ だけを見ていると 0 個と 1 個をどちらも取りこぼす。
tray_children() {
	xwininfo -display "$DISPLAY" -id "$tray" -children 2>/dev/null |
		awk '/ child(ren)?[:.]/ { print $1; exit }'
}
tray_width() {
	xwininfo -display "$DISPLAY" -id "$tray" 2>/dev/null |
		awk '/^  Width:/ { print $2; exit }'
}

# アイコンが 0 個の時点ではトレイは unmap されている（幅 0 相当）
w_before=$(tray_width)

# ------------------------------------------------------------------
# 1〜3. dock する
# ------------------------------------------------------------------
CLIENT_IN="$TMP/fifo"
mkfifo "$CLIENT_IN"
# fd 3 を書き込み側として開いたままにする（閉じると getchar が EOF になる）
"$TMP/trayclient" <"$CLIENT_IN" >"$TMP/icon.txt" 2>"$TMP/icon.err" &
TRAY_PID=$!
_LIB_PIDS="$_LIB_PIDS $TRAY_PID"
exec 3>"$CLIENT_IN"

icon=""
i=0
while [ $i -lt 60 ]; do
	icon=$(cat "$TMP/icon.txt" 2>/dev/null | head -n1)
	[ -n "$icon" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$icon" ] || fail "トレイクライアントがアイコンを作れませんでした: $(cat "$TMP/icon.err" 2>/dev/null)"

# reparent されるのを待つ
i=0
parent=""
while [ $i -lt 50 ]; do
	parent=$(get_parent_window "$icon")
	[ "$parent" = "$tray" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "$parent" = "$tray" ] ||
	fail "アイコンがトレイへ reparent されていません (parent=$parent, tray=$tray)"

nch=$(tray_children)
[ "${nch:-0}" -ge 1 ] || fail "トレイの子が 0 のままです"

# XEMBED_MAPPED が立っているので map されているはず
state=$(xwininfo -display "$DISPLAY" -id "$icon" 2>/dev/null |
	awk '/Map State:/ { print $3; exit }')
[ "$state" = "IsViewable" ] ||
	fail "_XEMBED_INFO で MAPPED を立てたのに map されていません (Map State: $state)"

# ------------------------------------------------------------------
# 4. トレイの幅が広がっている
# ------------------------------------------------------------------
w_after=$(tray_width)
[ "${w_after:-0}" -gt "${w_before:-0}" ] ||
	fail "アイコンを埋め込んでもトレイの幅が広がっていません ($w_before -> $w_after)"

# ------------------------------------------------------------------
# 5. MAPPED を落とすと unmap される
# ------------------------------------------------------------------
printf 'u' >&3
i=0
while [ $i -lt 30 ]; do
	state=$(xwininfo -display "$DISPLAY" -id "$icon" 2>/dev/null |
		awk '/Map State:/ { print $3; exit }')
	[ "$state" = "IsUnMapped" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "$state" = "IsUnMapped" ] ||
	fail "_XEMBED_INFO の MAPPED を落としても unmap されません (Map State: $state)"

printf 'm' >&3
i=0
while [ $i -lt 30 ]; do
	state=$(xwininfo -display "$DISPLAY" -id "$icon" 2>/dev/null |
		awk '/Map State:/ { print $3; exit }')
	[ "$state" = "IsViewable" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "$state" = "IsViewable" ] || fail "MAPPED を戻しても map されません"

# ------------------------------------------------------------------
# 6. アイコンを破棄するとトレイの幅が戻る
# ------------------------------------------------------------------
printf 'd' >&3
i=0
while [ $i -lt 30 ]; do
	nch=$(tray_children)
	[ "${nch:-1}" = "0" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "${nch:-1}" = "0" ] ||
	fail "アイコンを破棄してもトレイの子が残っています ($nch)"

w_gone=$(tray_width)
[ "${w_gone:-0}" -le "${w_before:-0}" ] ||
	fail "アイコンが消えてもトレイの幅が戻っていません ($w_after -> $w_gone)"

# ------------------------------------------------------------------
# 7. WM が落ちてもアイコンは残る（クライアントの資源。§4.8）
# ------------------------------------------------------------------
# もう一度 dock してから WM を止める
"$TMP/trayclient" </dev/null >"$TMP/icon2.txt" 2>/dev/null &
TRAY_PID2=$!
_LIB_PIDS="$_LIB_PIDS $TRAY_PID2"
icon2=""
i=0
while [ $i -lt 60 ]; do
	icon2=$(head -n1 "$TMP/icon2.txt" 2>/dev/null)
	[ -n "$icon2" ] && break
	sleep 0.2
	i=$((i + 1))
done
if [ -n "$icon2" ]; then
	i=0
	while [ $i -lt 50 ]; do
		[ "$(get_parent_window "$icon2")" = "$tray" ] && break
		sleep 0.2
		i=$((i + 1))
	done

	kill "$WM_PID" 2>/dev/null || true
	sleep 1.5

	# WM が終了してもウィンドウ自体は生きている（root へ戻っている）
	xwininfo -display "$DISPLAY" -id "$icon2" >/dev/null 2>&1 ||
		fail "WM の正常終了でトレイアイコンが破棄されました (他プロセスの資源を壊している)"

	root_id=$(get_root_window)
	[ "$(get_parent_window "$icon2")" = "$root_id" ] ||
		fail "WM の終了後、アイコンが root へ戻っていません"
else
	echo "# 2 回目の dock ができなかったため 7 は省略"
fi

# ------------------------------------------------------------------
# 8. kill -9 でもアイコンが残る（save-set。§3.1.1）
# ------------------------------------------------------------------
start_wm
i=0
tray=""
while [ $i -lt 40 ]; do
	tray=$(find_tray)
	[ -n "$tray" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$tray" ] || fail "WM 再起動後にトレイのコンテナが見つかりません"

"$TMP/trayclient" </dev/null >"$TMP/icon3.txt" 2>/dev/null &
TRAY_PID3=$!
_LIB_PIDS="$_LIB_PIDS $TRAY_PID3"
icon3=""
i=0
while [ $i -lt 60 ]; do
	icon3=$(head -n1 "$TMP/icon3.txt" 2>/dev/null)
	[ -n "$icon3" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$icon3" ] || fail "3 回目の dock でアイコンを作れませんでした"

i=0
while [ $i -lt 50 ]; do
	[ "$(get_parent_window "$icon3")" = "$tray" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ "$(get_parent_window "$icon3")" = "$tray" ] ||
	fail "3 回目の dock で reparent されていません"

kill -9 "$WM_PID" 2>/dev/null || true
sleep 1.5

xwininfo -display "$DISPLAY" -id "$icon3" >/dev/null 2>&1 ||
	fail "kill -9 でトレイアイコンが道連れになりました (save-set が効いていない)"

root_id=$(get_root_window)
[ "$(get_parent_window "$icon3")" = "$root_id" ] ||
	fail "kill -9 の後、save-set がアイコンを root へ戻していません"

exec 3>&-
echo "# XEmbed の dock / mapped / 取り外し / 正常終了と kill -9 の後始末を確認"
exit 0
