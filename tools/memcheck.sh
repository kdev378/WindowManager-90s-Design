#!/bin/sh
#
# memcheck.sh - SPEC §9.1 / §9.1.1 のメモリ計測 (M0 / M1 / M20 / M20h)
#
# 概要:
#   自前で Xvfb を起動し、その上で w98wm を条件を変えて起動し、
#   /proc/<pid>/smaps_rollup の Private_Dirty (指標A) を測る。
#   計測条件は SPEC §9.1 の表の通り 4 点:
#     M0   : 起動直後・ウィンドウ 0 枚・タスクバー無効        目標 < 400 KB
#     M1   : 起動直後・ウィンドウ 0 枚・タスクバー/トレイ有効  目標 < 550 KB
#     M20  : 20 ウィンドウ・タスクバー/トレイ有効
#            目標 (M20-M1)/20 < 2KB  かつ  M20 < 1024KB (AND 条件。SPEC §9.1)
#     M20h : M20 の状態で 1 時間 + 500 回の開閉/移動/リサイズ (--long 時のみ)
#            目標 M20h - M20 < 32 KB
#   さらに M20 の値に SPEC §9.1.1 の判断規則 (反証時の対応表) を適用して表示する。
#
# 設定 (タスクバー有効/無効) の伝え方についての注記:
#   本スクリプト作成時点では src/config.c (SPEC §8 の設定パーサ) は
#   まだ実装されていない。brand.h に明記された規約
#     ($XDG_CONFIG_HOME/<WM_CONFIG_DIR>/<WM_CONFIG_FILE>)
#   に沿って `taskbar=false` / `taskbar=true`+`tray=true` の設定ファイルを
#   一時ディレクトリに生成し、XDG_CONFIG_HOME 経由で渡す。
#   ディレクトリ名・ファイル名は src/brand.h から読み取り、ハードコードしない。
#   config.c が実装されるまでは WM 側がこれを無視するため M0 と M1 が
#   同じ値になることがあるが、これは仕様通りの過渡的な状態であり、
#   config.c 実装後は自動的に意味のある値になる。
#
# 使い方:
#   tools/memcheck.sh [--long] [--json]
#     --long  M20h も計測する (1時間かかる。nightly CI 向け)
#     --json  結果を JSON でも出力する (docs/MEMORY.md への追記に使う)
#
# 終了コード:
#   0   全ゲート PASS (非Linux環境でのスキップも含む)
#   1   いずれかのゲートが FAIL、または致命的エラー (バイナリ無し等)
#   2   引数エラー
#
# シェル: POSIX sh (dash で動作確認済み)。

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT_DIR/w98wm"
BRAND_H="$ROOT_DIR/src/brand.h"

LONG=0
JSON=0
for arg in "$@"; do
	case "$arg" in
	--long)
		LONG=1
		;;
	--json)
		JSON=1
		;;
	-h | --help)
		echo "使い方: $0 [--long] [--json]"
		echo "  --long  M20h (1時間 + 500サイクル) も計測する"
		echo "  --json  結果を機械可読な JSON でも出力する"
		exit 0
		;;
	*)
		echo "不明なオプション: $arg" >&2
		exit 2
		;;
	esac
done

# --- 指標A (Private_Dirty) は Linux 専用 (SPEC §9.1) ---
if [ ! -r /proc/self/smaps_rollup ]; then
	echo "指標A (Private_Dirty, /proc/*/smaps_rollup) はこの環境では利用できません。"
	echo "SPEC §9.1 の通り、指標Aは Linux でのみ測定可能です。この環境では計測をスキップします。"
	exit 0
fi

# --- バイナリが無ければ穏やかに失敗する (他エージェントが src/ を実装中の可能性) ---
if [ ! -x "$BIN" ]; then
	echo "エラー: $BIN が見つかりません。" >&2
	echo "まだビルドされていないようです (他の担当がまだ src/ を実装中の可能性があります)。" >&2
	echo "先に 'make' を実行してから再度お試しください。" >&2
	exit 1
fi

for tool in Xvfb xterm; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "エラー: 計測に必要なツール '$tool' が見つかりません。" >&2
		exit 1
	fi
done

# --- brand.h から設定ディレクトリ/ファイル名を読む (ハードコードしない) ---
CONFIG_DIR_NAME=$(sed -n 's/^#define[[:space:]]\+WM_CONFIG_DIR[[:space:]]\+"\([^"]*\)".*/\1/p' "$BRAND_H" 2>/dev/null | head -n1)
CONFIG_FILE_NAME=$(sed -n 's/^#define[[:space:]]\+WM_CONFIG_FILE[[:space:]]\+"\([^"]*\)".*/\1/p' "$BRAND_H" 2>/dev/null | head -n1)
: "${CONFIG_DIR_NAME:=w98wm}"
: "${CONFIG_FILE_NAME:=config}"

WORKROOT=$(mktemp -d)
XVFB_PID=""
WM_PID=""
XTERM_PIDS=""

cleanup() {
	for p in $XTERM_PIDS; do
		kill "$p" 2>/dev/null || true
	done
	if [ -n "$WM_PID" ]; then
		kill "$WM_PID" 2>/dev/null || true
	fi
	if [ -n "$XVFB_PID" ]; then
		kill "$XVFB_PID" 2>/dev/null || true
	fi
	wait 2>/dev/null || true
	rm -rf "$WORKROOT"
}
trap cleanup EXIT INT TERM

find_free_display() {
	d=99
	while [ -e "/tmp/.X${d}-lock" ]; do
		d=$((d + 1))
	done
	echo "$d"
}

DISP=$(find_free_display)
DISPLAY=":$DISP"
export DISPLAY

Xvfb "$DISPLAY" -screen 0 1280x1024x24 -nolisten tcp >"$WORKROOT/xvfb.log" 2>&1 &
XVFB_PID=$!

i=0
while [ ! -e "/tmp/.X${DISP}-lock" ] && [ $i -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
if [ ! -e "/tmp/.X${DISP}-lock" ]; then
	echo "エラー: Xvfb の起動に失敗しました (ログ: $WORKROOT/xvfb.log)" >&2
	exit 1
fi

# start_wm <設定ファイルの中身> <連番>
# 成功したら WM_PID を設定して 0 を返す。すぐ終了したら WM_PID を空にして 1 を返す。
start_wm() {
	body="$1"
	idx="$2"
	cfgroot="$WORKROOT/cfg_$idx"
	mkdir -p "$cfgroot/$CONFIG_DIR_NAME"
	printf '%s\n' "$body" >"$cfgroot/$CONFIG_DIR_NAME/$CONFIG_FILE_NAME"
	XDG_CONFIG_HOME="$cfgroot" DISPLAY="$DISPLAY" "$BIN" >"$WORKROOT/wm_$idx.log" 2>&1 &
	WM_PID=$!
	sleep 1.5
	if ! kill -0 "$WM_PID" 2>/dev/null; then
		echo "警告: WM がこの条件ですぐに終了しました (ログ: $WORKROOT/wm_$idx.log)。" >&2
		WM_PID=""
		return 1
	fi
	return 0
}

stop_wm() {
	if [ -n "$WM_PID" ]; then
		kill "$WM_PID" 2>/dev/null || true
		wait "$WM_PID" 2>/dev/null || true
		WM_PID=""
	fi
}

measure_pd() {
	pid="$1"
	if [ -z "$pid" ] || [ ! -r "/proc/$pid/smaps_rollup" ]; then
		return 1
	fi
	awk '/^Private_Dirty:/ { sum += $2 } END { printf "%d\n", sum + 0 }' "/proc/$pid/smaps_rollup"
}

run_stress_cycles() {
	# M20h: 1時間に 500 回の開閉/移動/リサイズを均等配分して実行する。
	total=500
	seconds=3600
	interval=$(awk "BEGIN { printf \"%.3f\", $seconds / $total }")
	have_xdotool=0
	command -v xdotool >/dev/null 2>&1 && have_xdotool=1

	idx=0
	while [ $idx -lt $total ]; do
		if [ "$have_xdotool" -eq 1 ]; then
			slot=$(((idx % 20) + 1))
			win=$(xdotool search --onlyvisible --name '.*' 2>/dev/null | sed -n "${slot}p")
			if [ -n "$win" ]; then
				case $((idx % 3)) in
				0)
					xdotool windowmove --sync "$win" $(((idx * 7) % 800)) $(((idx * 5) % 600)) >/dev/null 2>&1 || true
					;;
				1)
					xdotool windowsize --sync "$win" $((300 + (idx % 200))) $((200 + (idx % 150))) >/dev/null 2>&1 || true
					;;
				2)
					xdotool windowkill "$win" >/dev/null 2>&1 || true
					xterm -display "$DISPLAY" >/dev/null 2>&1 &
					XTERM_PIDS="$XTERM_PIDS $!"
					;;
				esac
			fi
		fi
		sleep "$interval"
		idx=$((idx + 1))
	done
}

echo "=== w98wm memcheck (SPEC §9.1) ==="
echo "DISPLAY=$DISPLAY"
echo "設定ディレクトリ名 (brand.h より): $CONFIG_DIR_NAME/$CONFIG_FILE_NAME"
echo ""

M0_KB=""
M1_KB=""
M20_KB=""
M20H_KB=""

# --- M0: ウィンドウ 0 枚・タスクバー無効 ---
echo "-- M0 を計測中 (タスクバー無効) --"
if start_wm "taskbar=false" 0; then
	M0_KB=$(measure_pd "$WM_PID") || M0_KB=""
fi
stop_wm

# --- M1: ウィンドウ 0 枚・タスクバー/トレイ有効 ---
echo "-- M1 を計測中 (タスクバー/トレイ有効) --"
if start_wm "taskbar=true
tray=true" 1; then
	M1_KB=$(measure_pd "$WM_PID") || M1_KB=""
fi
stop_wm

# --- M20: 20 ウィンドウ・タスクバー/トレイ有効 ---
echo "-- M20 を計測中 (xterm 20枚) --"
if start_wm "taskbar=true
tray=true" 2; then
	j=0
	while [ $j -lt 20 ]; do
		xterm -display "$DISPLAY" >/dev/null 2>&1 &
		XTERM_PIDS="$XTERM_PIDS $!"
		j=$((j + 1))
	done

	if command -v xdotool >/dev/null 2>&1; then
		k=0
		n=0
		while [ $k -lt 100 ]; do
			n=$(xdotool search --onlyvisible --name '.*' 2>/dev/null | wc -l | tr -d ' ')
			if [ "${n:-0}" -ge 20 ] 2>/dev/null; then
				break
			fi
			sleep 0.2
			k=$((k + 1))
		done
	else
		sleep 5
	fi
	sleep 1

	M20_KB=$(measure_pd "$WM_PID") || M20_KB=""

	if [ "$LONG" -eq 1 ] && [ -n "$M20_KB" ]; then
		echo "-- M20h を計測中 (1時間 + 500 サイクル。しばらくお待ちください) --" >&2
		run_stress_cycles
		M20H_KB=$(measure_pd "$WM_PID") || M20H_KB=""
	fi
fi
for p in $XTERM_PIDS; do
	kill "$p" 2>/dev/null || true
done
XTERM_PIDS=""
stop_wm

kill "$XVFB_PID" 2>/dev/null || true
wait "$XVFB_PID" 2>/dev/null || true
XVFB_PID=""

# --- 判定 ---
GATE_FAIL=0

fmt() {
	[ -n "$1" ] && echo "$1" || echo "N/A"
}

if [ -n "$M0_KB" ]; then
	if [ "$M0_KB" -lt 400 ]; then M0_PASS=PASS; else
		M0_PASS=FAIL
		GATE_FAIL=1
	fi
else
	M0_PASS=FAIL
	GATE_FAIL=1
fi

if [ -n "$M1_KB" ]; then
	if [ "$M1_KB" -lt 550 ]; then M1_PASS=PASS; else
		M1_PASS=FAIL
		GATE_FAIL=1
	fi
else
	M1_PASS=FAIL
	GATE_FAIL=1
fi

if [ -n "$M20_KB" ] && [ -n "$M1_KB" ]; then
	MARGIN=$((M20_KB - M1_KB))
	# (M20-M1)/20 < 2KB  <=>  M20-M1 < 40  (整数のまま比較。20は正の定数なので同値)
	if [ "$MARGIN" -lt 40 ] && [ "$M20_KB" -lt 1024 ]; then
		M20_PASS=PASS
	else
		M20_PASS=FAIL
		GATE_FAIL=1
	fi
else
	M20_PASS=FAIL
	GATE_FAIL=1
fi

M20H_PASS="-"
if [ "$LONG" -eq 1 ]; then
	if [ -n "$M20H_KB" ] && [ -n "$M20_KB" ]; then
		DELTA=$((M20H_KB - M20_KB))
		if [ "$DELTA" -lt 32 ]; then M20H_PASS=PASS; else
			M20H_PASS=FAIL
			GATE_FAIL=1
		fi
	else
		M20H_PASS=FAIL
		GATE_FAIL=1
	fi
fi

echo ""
echo "条件   実測値(KB)  目標                                     判定"
echo "-----  ----------  ---------------------------------------  ----"
printf '%-5s  %10s  %-41s  %s\n' "M0" "$(fmt "$M0_KB")" "< 400 KB" "$M0_PASS"
printf '%-5s  %10s  %-41s  %s\n' "M1" "$(fmt "$M1_KB")" "< 550 KB" "$M1_PASS"
printf '%-5s  %10s  %-41s  %s\n' "M20" "$(fmt "$M20_KB")" "(M20-M1)/20<2KB かつ M20<1024KB" "$M20_PASS"
if [ "$LONG" -eq 1 ]; then
	printf '%-5s  %10s  %-41s  %s\n' "M20h" "$(fmt "$M20H_KB")" "M20h-M20 < 32 KB" "$M20H_PASS"
else
	echo "M20h   (未計測。--long を指定すると計測します)"
fi

# --- SPEC §9.1.1 の判断規則 ---
echo ""
echo "--- SPEC §9.1.1 判定 (M20 実測値に基づく) ---"
if [ -n "$M20_KB" ]; then
	if [ "$M20_KB" -lt 400 ]; then
		echo "M20 = ${M20_KB} KB (< 400 KB): 目標は達成可能。そのまま全機能を実装する。"
	elif [ "$M20_KB" -lt 700 ]; then
		echo "M20 = ${M20_KB} KB (400-700 KB): 達成は微妙。残りのフェーズで実測しながら進め、Phase 4 完了時に再判定する。"
	elif [ "$M20_KB" -lt 1024 ]; then
		echo "M20 = ${M20_KB} KB (700 KB-1MB): 全機能では超過が濃厚。削減順序を発動する:"
		echo "  1) システムトレイ 2) スタートメニュー 3) タスクバー 4) 仮想デスクトップ多面対応 5) 配色プリセット"
	else
		echo "M20 = ${M20_KB} KB (> 1MB): 設計前提が誤り。目標を実測値ベースで再設定し、§11 決定#1 を撤回して再承認を得る。"
	fi
else
	echo "M20 が計測できなかったため判定できません。"
fi

if [ "$JSON" -eq 1 ]; then
	json_num() {
		[ -n "$1" ] && echo "$1" || echo "null"
	}
	json_bool() {
		case "$1" in
		PASS) echo "true" ;;
		FAIL) echo "false" ;;
		*) echo "null" ;;
		esac
	}
	echo ""
	echo "--- JSON (docs/MEMORY.md 追記用) ---"
	cat <<JSON_EOF
{
  "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "m0_kb": $(json_num "$M0_KB"),
  "m1_kb": $(json_num "$M1_KB"),
  "m20_kb": $(json_num "$M20_KB"),
  "m20h_kb": $(json_num "$M20H_KB"),
  "m0_pass": $(json_bool "$M0_PASS"),
  "m1_pass": $(json_bool "$M1_PASS"),
  "m20_pass": $(json_bool "$M20_PASS"),
  "m20h_pass": $(json_bool "$M20H_PASS")
}
JSON_EOF
fi

if [ "$GATE_FAIL" -ne 0 ]; then
	echo ""
	echo "結果: FAIL (少なくとも1つのゲートを満たしていません)"
	exit 1
fi

echo ""
echo "結果: PASS"
exit 0
