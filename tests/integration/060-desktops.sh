#!/bin/sh
#
# 060-desktops.sh
#
# _NET_NUMBER_OF_DESKTOPS / _NET_CURRENT_DESKTOP / _NET_CLIENT_LIST_STACKING
#
# SPEC §5.2.1 の規約により、_NET_SUPPORTED に載せるアトムには対応するテストが
# 要る。ここは「正しい形式で存在し、値が仕様の範囲に収まり、2 つのクライアント
# リストが整合しているか」だけを見る軽量な確認。§5.2.1 は「テストの重さで実装が
# 止まるのは本末転倒なので、厳密さより存在することを優先する」と定めている。
#
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib.sh"

require_tool xterm

start_xvfb
start_wm

# --- _NET_NUMBER_OF_DESKTOPS -------------------------------------------
ndesk=$(xprop -display "$DISPLAY" -root _NET_NUMBER_OF_DESKTOPS 2>/dev/null |
	sed 's/.*= *//' | tr -d ' ')
[ -n "$ndesk" ] || fail "_NET_NUMBER_OF_DESKTOPS がありません"
case "$ndesk" in
'' | *[!0-9]*) fail "_NET_NUMBER_OF_DESKTOPS が数値ではありません: '$ndesk'" ;;
esac
# 既定は 1 面、設定で 1..16 (SPEC §3.8)
[ "$ndesk" -ge 1 ] && [ "$ndesk" -le 16 ] ||
	fail "_NET_NUMBER_OF_DESKTOPS が範囲外です: $ndesk"

# --- _NET_CURRENT_DESKTOP ----------------------------------------------
cur=$(xprop -display "$DISPLAY" -root _NET_CURRENT_DESKTOP 2>/dev/null |
	sed 's/.*= *//' | tr -d ' ')
[ -n "$cur" ] || fail "_NET_CURRENT_DESKTOP がありません"
case "$cur" in
'' | *[!0-9]*) fail "_NET_CURRENT_DESKTOP が数値ではありません: '$cur'" ;;
esac
[ "$cur" -lt "$ndesk" ] ||
	fail "_NET_CURRENT_DESKTOP($cur) が _NET_NUMBER_OF_DESKTOPS($ndesk) 以上です"

# --- _NET_CLIENT_LIST_STACKING -----------------------------------------
# ウィンドウ 0 枚でもプロパティ自体は存在すること
xprop -display "$DISPLAY" -root _NET_CLIENT_LIST_STACKING >/dev/null 2>&1 ||
	fail "_NET_CLIENT_LIST_STACKING がありません"

xterm -display "$DISPLAY" -geometry 40x10+30+30 >/dev/null 2>&1 &
_LIB_PIDS="$_LIB_PIDS $!"
sleep 2
xterm -display "$DISPLAY" -geometry 40x10+90+90 >/dev/null 2>&1 &
_LIB_PIDS="$_LIB_PIDS $!"
sleep 3

list=$(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# *//')
stack=$(xprop -display "$DISPLAY" -root _NET_CLIENT_LIST_STACKING 2>/dev/null | sed 's/.*# *//')

n_list=$(printf '%s' "$list" | tr ',' '\n' | grep -c '0x' || true)
n_stack=$(printf '%s' "$stack" | tr ',' '\n' | grep -c '0x' || true)

[ "$n_list" -ge 2 ] ||
	fail "_NET_CLIENT_LIST に 2 窓が載っていません (n=$n_list): $list"

# 2 つのリストは同じ集合でなければならない。
# 順序は別物である点に注意: _NET_CLIENT_LIST は生成順、
# _NET_CLIENT_LIST_STACKING は実スタック順 (SPEC §3.7.1)。
[ "$n_list" = "$n_stack" ] ||
	fail "要素数が違います: _NET_CLIENT_LIST=$n_list _NET_CLIENT_LIST_STACKING=$n_stack"

for w in $(printf '%s' "$list" | tr ',' ' '); do
	case "$stack" in
	*"$w"*) : ;;
	*) fail "$w が _NET_CLIENT_LIST_STACKING にありません" ;;
	esac
done

echo "OK: デスクトップ属性 (面数=$ndesk 現在=$cur) と"
echo "    _NET_CLIENT_LIST_STACKING ($n_stack 窓、_NET_CLIENT_LIST と同集合) を確認しました"
exit 0
