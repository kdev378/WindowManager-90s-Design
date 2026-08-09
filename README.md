# w98wm

Windows 98 のルック＆フィールを再現する、X11 用の超軽量リパレンティング型ウィンドウマネージャ。
現代のアプリケーション（GTK3/4・Qt5/6・Electron・Firefox・Chromium）が正常に動くことを
前提条件としつつ、常駐メモリ 1 MB 未満を目指します。

対象: **Linux**（glibc / musl）と **OpenBSD**。ウィンドウシステムは **X11** のみ。

- 依存は libxcb 系のみ（Xlib / GTK / cairo / fontconfig を使いません）
- ICCCM 2.0 / EWMH 1.5 / Motif WM Hints に準拠
- タスクバー・スタートメニュー・システムトレイを内蔵（無効化可能）

ビルドプロファイルは 2 つあります。**core**（既定）は libxcb 系のみで、メモリ目標が
適用されます。**xft**（`make XFT=1`）はアンチエイリアスと確実な多言語表示が得られる
代わりに、fontconfig/FreeType 依存となりメモリ目標の対象外です。
core ビルドでの日本語タイトル表示は、環境にある X コアフォント次第の best-effort です
（詳細は SPEC §4.5.5）。

## ドキュメント

| 文書 | 内容 |
| --- | --- |
| [docs/SPEC.md](docs/SPEC.md) | 仕様書（設計方針、外観・挙動の詳細、対応プロトコル、メモリ目標） |
| [docs/PLAN.md](docs/PLAN.md) | 開発計画（7 フェーズの工程、検証戦略、リスク） |
| [docs/TRACEABILITY.md](docs/TRACEABILITY.md) | 要求 → 実装 → 検証の対応表。**埋まっていない穴も明記** |
| [docs/MANUAL-TEST.md](docs/MANUAL-TEST.md) | 実環境での手動検証の手引き（GUI 環境の作り方から） |
| [docs/MEMORY.md](docs/MEMORY.md) | メモリ実測の記録と、計測方法を 2 度直した経緯 |

## 状態

**Phase 4 完了** — タスクバー・スタートメニュー・システムトレイ・Alt+Tab まで動きます。

```
make          # ビルド (依存: libxcb, xcb-randr, xcb-sync, xcb-keysyms, xcb-shape)
make test     # Xvfb 上で結合テスト (16 本)
make memcheck # メモリ実測 (SPEC §9.1)
```

動くもの: Win98 の枠・タイトルバー・キャプションボタン・ウィンドウメニュー・カーソル、
ウィンドウの管理と reparent、ICCCM 2.0 / EWMH 1.5 / Motif hints、
フォーカス（ICCCM の 4 入力モデル）、移動・リサイズ（レート制御と `_NET_WM_SYNC_REQUEST`）、
スタッキングとレイヤ、キーバインド、複数デスクトップ、マルチモニタ (RandR)、
`_NET_WM_PING`、アイコン、CSD、Shape、
**タスクバー（strut・タスクボタン・時計・トレイ）、スタートメニュー、Alt+Tab スイッチャ**。

まだ無いもの: スタートメニューの階層化、タスクバーのオートハイドとスクロール矢印、
最小化のズームアニメーション、デスクトップの右クリックメニュー、
`Super` 単押しでの起動（タップ判定）。OpenBSD への移植は Phase 5。
各項目の状態は [docs/PLAN.md](docs/PLAN.md) の「Phase 4 の結果」を参照。

何がどこまで検証されているかは [docs/TRACEABILITY.md](docs/TRACEABILITY.md)
に一覧があります（**未検証の 12 項目も優先度付きで明記**）。
実環境での手動検証は [docs/MANUAL-TEST.md](docs/MANUAL-TEST.md)。

| 指標 | 実測 | 目標 | 判定 |
| --- | --- | --- | --- |
| Private_Dirty (0 窓・タスクバー無効) | 248 KB | < 400 KB | PASS |
| Private_Dirty (0 窓・タスクバー/トレイ有効) | 252 KB | < 550 KB | PASS |
| Private_Dirty (20 窓) | 276 KB | < 1024 KB | PASS |
| 窓あたりの限界費用 | 1.2 KB | < 2 KB | PASS |
| 1 時間 + 500 回の開閉/移動/リサイズ後 | 272 KB (**−4 KB**) | 増分 < 32 KB | PASS |

**絶対値は起動ごとに 250–370 KB の幅で揺れます**（glibc のアリーナ初期状態と ASLR による）。
Phase 4 で計測方法の欠陥（限界費用を別プロセス間の差で測っていた）を直したため、
Phase 1–3 の記録値とは直接比較できません。経緯は [docs/MEMORY.md](docs/MEMORY.md)。

## 設定

`$XDG_CONFIG_HOME/w98wm/config`（無ければ `~/.config/w98wm/config`）。
スタートメニューの項目は同じディレクトリの `menu` に置きます:

```
# ラベル | コマンド 、"-" だけの行はセパレータ
プログラム(P)                 | xterm
-
ファイル名を指定して実行(R)   | xterm -e sh
```

名称は実装が動いてから決めるため、現在の `w98wm` は仮称です（SPEC §11.1）。
