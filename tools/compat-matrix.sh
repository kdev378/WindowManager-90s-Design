#!/bin/sh
#
# compat-matrix.sh — SPEC §7.1 の検証対象アプリを順に起動し、
# 手動確認用のチェックリストを Markdown で出す (PLAN.md Phase 0)。
#
# 使い方（w98wm が動いている X セッションの端末で）:
#   sh tools/compat-matrix.sh            # インストール済みのアプリを 1 つずつ起動
#   sh tools/compat-matrix.sh --list     # 起動せず、チェックリストだけ出す
#   sh tools/compat-matrix.sh --keep     # 起動したアプリを閉じずに残す
#   sh tools/compat-matrix.sh > result.md
#
# 自動で確かめるのは「起動して WM の管理下に入ったか」(_NET_CLIENT_LIST)
# だけ。見た目と操作感は人が見る。各アプリの「確認すること」を表に出すので、
# 結果欄に ✅ / ❌ / メモを書き込んで報告に使う（docs/MANUAL-TEST.md）。
#
set -u

MODE=run
for a in "$@"; do
	case "$a" in
	--list) MODE=list ;;
	--keep) KEEP=1 ;;
	-h|--help)
		sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
		exit 0 ;;
	*) echo "compat-matrix.sh: 不明な引数: $a" >&2; exit 2 ;;
	esac
done
KEEP=${KEEP:-0}

if [ "$MODE" = run ]; then
	[ -n "${DISPLAY:-}" ] || { echo "DISPLAY が設定されていません" >&2; exit 1; }
	command -v xprop >/dev/null 2>&1 || { echo "xprop が必要です (x11-utils)" >&2; exit 1; }
	xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1 ||
		echo "# 注意: EWMH の WM が動いていないようです。w98wm の下で実行してください" >&2
fi

# 行の形式: 区分|表示名|起動コマンド（先頭語で存在を判定）|確認すること
# 確認すること は SPEC §7 の該当節に対応する。
MATRIX='Firefox|Firefox|firefox --new-instance --no-remote about:blank|サイト全画面 (F11 / 動画の全画面)・PiP 窓が最前面・ポップアップの位置
Chromium|Chromium|chromium --user-data-dir=/tmp/w98wm-compat-chromium about:blank|全画面・PiP・ダウンロード等のダイアログ
Chromium|Chromium (snap 名)|chromium-browser --user-data-dir=/tmp/w98wm-compat-chromium about:blank|同上
GTK3|gedit|gedit|CSD の有無 / 最大化・復元 / 保存ダイアログが親の上に出る (§7.2)
GTK3|nautilus|nautilus --new-window|CSD のヘッダバー: ドラッグ移動・右クリックでウィンドウメニュー (_GTK_SHOW_WINDOW_MENU)
GTK4|gnome-text-editor|gnome-text-editor|GTK4 CSD: 最大化で見かけの矩形が作業領域に一致 (§7.2)
Qt|qterminal|qterminal|リサイズのちらつき (_NET_WM_SYNC_REQUEST, §7.3)
Qt|VLC|vlc|全画面切替・ダイアログ
Electron|VS Code|code --new-window|CSD/SSD どちらで出るか・最大化・全画面
Java/Swing|SwingTest|java -cp tools tools/SwingTest.java|クリックでフォーカスが移る (globally-active, §3.6)
SDL2|SDL2 ゲーム|SDL_TEST_CMD|全画面切替でタスクバーが隠れる・戻る
端末|xterm|xterm|リサイズ増分 (WM_NORMAL_HINTS の width_inc) で端数が出ない
端末|alacritty|alacritty|同上
動画|mpv|mpv --idle=yes --force-window=yes|全画面 (f) の出入り
ダイアログ|GIMP|gimp|多数のダイアログ・transient の重なり順・ドッキング窓'

client_list() {
	xprop -root _NET_CLIENT_LIST 2>/dev/null | sed 's/.*# *//' | tr ',' '\n' | tr -d ' ' | grep -v '^$' || true
}

echo "# w98wm 互換性マトリクス (SPEC §7.1)"
echo
if [ "$MODE" = run ]; then
	echo "- 日時: $(date '+%Y-%m-%d %H:%M')"
	echo "- WM: $(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | sed 's/.*# *//')"
	echo "- 自動判定: 起動して _NET_CLIENT_LIST に載ったか (15 秒以内)"
	echo
fi
echo "| 区分 | アプリ | 起動 | 確認すること | 結果 |"
echo "| --- | --- | --- | --- | --- |"

echo "$MATRIX" | while IFS='|' read -r cat name cmd check; do
	[ -n "$cat" ] || continue
	bin=${cmd%% *}
	if [ "$bin" = SDL_TEST_CMD ]; then
		cmd=${W98WM_SDL_CMD:-}
		bin=${cmd%% *}
		[ -n "$cmd" ] || {
			echo "| $cat | $name | (W98WM_SDL_CMD で指定) | $check | |"
			continue
		}
	fi
	if [ "$bin" = java ] && [ ! -f tools/SwingTest.java ]; then
		echo "| $cat | $name | (tools/SwingTest.java が無い) | $check | |"
		continue
	fi
	if ! command -v "$bin" >/dev/null 2>&1; then
		echo "| $cat | $name | 未インストール | $check | |"
		continue
	fi
	if [ "$MODE" = list ]; then
		echo "| $cat | $name | \`$cmd\` | $check | |"
		continue
	fi

	before=$(client_list)
	# 意図的に単語分割する（起動コマンドは引数を含む）
	# shellcheck disable=SC2086
	$cmd >/dev/null 2>&1 &
	pid=$!
	got=""
	i=0
	while [ $i -lt 75 ]; do
		for w in $(client_list); do
			echo "$before" | grep -qx "$w" || { got=$w; break; }
		done
		[ -n "$got" ] && break
		sleep 0.2
		i=$((i + 1))
	done
	if [ -n "$got" ]; then
		status="✅ 管理下 ($got)"
	elif kill -0 "$pid" 2>/dev/null; then
		status="❌ 15 秒以内に管理下に入らない"
	else
		status="❌ 起動直後に終了"
	fi
	echo "| $cat | $name | $status | $check | |"

	if [ "$KEEP" -eq 0 ]; then
		# まず行儀よく閉じる (_NET_CLOSE_WINDOW)。残ったら止める
		[ -n "$got" ] && command -v wmctrl >/dev/null 2>&1 && wmctrl -ic "$got" 2>/dev/null
		sleep 1
		kill "$pid" 2>/dev/null || true
	fi
done

echo
echo "結果欄に ✅ / ❌ / 気づいたことを書き、docs/MANUAL-TEST.md の報告に添えてください。"
