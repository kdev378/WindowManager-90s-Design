# w98wm 開発計画

[仕様書 (SPEC.md)](SPEC.md) を実装するための工程計画。

---

## 0. 全体像

7 フェーズ。各フェーズ末に「動く状態」と検証手段を置き、メモリ計測を毎フェーズ実施する。
各フェーズは 1 コミット単位ではなく、レビュー可能な粒度で複数コミットに分割する。

| Phase | 内容 | 成果物 | 概算規模 |
| --- | --- | --- | --- |
| 0 | 土台 | ビルドシステム、CI、Xephyr 検証環境、計測スクリプト | 約 400 行 |
| 1 | コア WM | reparent / ICCCM / フォーカス / 移動 / リサイズ / ペーシング / スタッキング | 約 1,900 行 |
| 2 | Win98 外観 | 描画プリミティブ、フレーム、ボタン、フォント解決、ウィンドウメニュー | 約 1,750 行 |
| 3 | EWMH 完全対応 | 全 `_NET_*`、RandR、strut、全画面、sync request、Motif/CSD | 約 1,550 行 |
| 4 | シェル | タスクバー、スタートメニュー、Alt+Tab、デスクトップ、トレイ | 約 1,800 行 |
| 5 | 移植と最適化 | OpenBSD 対応、pledge/unveil、musl 静的、メモリ削減 | 約 500 行 |
| 6 | 仕上げ | 設定、ドキュメント、パッケージ、互換性検証、安定化 | 約 400 行 |

合計 約 8,300 行（C）。ヘッダ・テスト・スクリプトを含めて約 10,000 行を見込む。
Phase 2 の増分はフォントのフォールバックチェーンと内蔵フォント、
Phase 3 の減分は Xinerama 経路を実装しないと確定したこと（SPEC §5.3）による。

---

## Phase 0 — 土台

**目的**: 以降の全フェーズで「作る → 動かす → 測る」を 1 コマンドで回せるようにする。

- `Makefile`（POSIX make。GNU make / BSD make 双方で動くこと。cmake/meson は使わない）
  - `make`（core プロファイル）, `make DEBUG=1`, `make XFT=1`, `make STATIC=1`, `make install`
  - 依存検出は `pkg-config` があれば使い、無ければ既定パスにフォールバック
  - **必須依存**: `xcb`, `xcb-randr`, `xcb-sync`, `xcb-keysyms`
    （`xcb-sync` は `_NET_WM_SYNC_REQUEST` に必要。SPEC §1.4）
  - **任意依存**: `xcb-shape`, `xcb-xfixes`（無い場合は該当機能を無効化してビルド継続）
- `src/atoms.c` — アトムテーブルと `implemented` フラグ（SPEC §5.2.1）。
  `_NET_SUPPORTED` はこのフラグから生成する。フェーズ進行に伴って昇格させる
- `src/compat.h` — Linux / OpenBSD の差分吸収（`strlcpy`, `pledge`, `posix_spawn`, poll）
- `src/util.c` — スラブアロケータ、ログ（`-v` で stderr、既定は無出力）
- `tools/run-xephyr.sh` — Xephyr 上で w98wm を起動し、テスト用アプリを配置する
- `tools/memcheck.sh` — SPEC §9.1 の **M0 / M1 / M20 / M20h** の 4 条件を測定し表形式で出力。
  0 枚の測定（M0/M1）を Phase 0 の時点から取り始めることで、以降のフェーズで
  「常駐の底が上がった」のか「ウィンドウ単価が増えた」のかを毎回切り分けられるようにする
- `tools/compat-matrix.sh` — §7.1 のアプリを順に起動し、手動確認用チェックリストを出す
- `tools/check-supported.sh` — `_NET_SUPPORTED` に載っている各アトムに対応する結合テストが
  存在するかを機械的に検査する（SPEC §5.2.1。テストの無いアトムが載っていたら CI 失敗）
- GitHub Actions: Ubuntu（gcc/clang, glibc/musl）でビルド + Xvfb 起動テスト + メモリ閾値チェック
  - OpenBSD は CI に無いため、`make -n` 相当の構文検査 + 手動検証手順を記載

**完了条件**: 何もしないバイナリが X に接続し、ルートウィンドウのイベントを受け、
`memcheck.sh` が数値を出す。

---

## Phase 1 — コア WM（ここで「使える」状態になる）

1. マネージャセレクション取得（`WM_S0`）、`SubstructureRedirect` の取得、
   既に WM が居る場合のエラー処理と `--replace`
2. 既存ウィンドウの adopt（起動時に mapped な子を走査して管理下に置く）
3. `MapRequest` → フレーム生成 → reparent → map の一連
4. `ConfigureRequest` / `ConfigureNotify` / `UnmapNotify` / `DestroyNotify` / `PropertyNotify`
   のハンドリング。**Unmap の同期問題**（自分の reparent 由来の UnmapNotify を無視する
   カウンタ管理）を最初に正しく作る — ここを雑にすると後で必ず「ウィンドウが消える」バグになる
5. ICCCM: `WM_STATE`, `WM_NORMAL_HINTS`（グラビティ・増分含む）, `WM_HINTS`,
   `WM_PROTOCOLS`, `WM_TRANSIENT_FOR`, `WM_CHANGE_STATE`
6. フォーカス（click / sloppy、`WM_TAKE_FOCUS`、MRU 履歴）
7. マウスによる移動・リサイズ（ポインタ grab、8 方向、増分・最小最大の適用）
8. **ドラッグ中のフレームペーシング（SPEC §3.4.1）をこの段階で作り込む** —
   モーション合体・16ms レート上限・「未確定リクエストは 1 つまで」のフックを最初から
   入れておく。ここを素朴に書いて後から直すと、イベントループ全体の構造に手が入るため。
   同期カウンタ（§7.3）は Phase 3 で差し込むが、その差し込み口だけ用意しておく
9. スタッキング（SPEC §3.7 のレイヤ + transient チェーン。
   **`transient_for` の循環検出を最初から入れる** — 無防備だと無限ループでハングする）
10. キーバインド基盤（`XGrabKey`、`MappingNotify` 対応）と `exec`
    （`posix_spawn` + SIGCHLD を `SIG_IGN`、子側は `POSIX_SPAWN_SETSIGDEF` で戻す。
    SPEC §2.2.1）
11. 終了時のクリーンアップ（全クライアントを root へ reparent し直す）

**この時点の見た目**: 装飾は単色の灰色矩形（プレースホルダ）。
**完了条件**:
- xterm と firefox を起動し、移動・リサイズ・フォーカス・閉じるが破綻なく動く
- w98wm を kill してもウィンドウが消えない
- **低速リンク検証**：`ssh -X` 相当（`tc` で 30ms 遅延・1Mbps に絞った環境、
  もしくは `x11vnc` 経由）でドラッグしてカクつかないこと。ペーシングの検証は
  この段階で終わらせる

**主なリスク**: `UnmapNotify` の扱いと `ConfigureRequest` のグラビティ計算。
ここは仕様書（ICCCM 4.1.5）と実装（Openbox の該当箇所）を突き合わせて丁寧にやる。

---

## Phase 2 — Windows 98 外観

1. `theme.c` — 配色テーブル、メトリクス、プリセット
2. `draw.c` — `fill`, `line`, `bevel`(4 種), `gradient_h`, `text`, `bitmap_glyph`
3. `font.c` — **SPEC §4.5 のフォールバックチェーンを最初に実装する**（後付けにしない）。
   `ListFonts` による候補照会 → `xcb_open_font_checked` + `request_check` による確実な検証 →
   CJK 補助フォント → 内蔵ビットマップフォントまでを一続きで作る。
   `ImageText16`（UCS-2 変換）、`QueryTextExtents` による幅計測（§4.5.2 の
   `QueryFont` 回避ルールを守る）、省略表示（`...`）。UTF-8 → UCS-2 変換は自前（約 40 行）。
   **検証**：フォントパスを空にした X サーバ（`xset fp= rehash` / `Xvfb -fp ""`）でも
   起動して文字が出ることを、この段階のテストに含める
4. `deco.c` — フレーム全体の描画。`Expose` の矩形単位で必要な部分だけ再描画
5. キャプションボタン（16×14）と 1bit グリフ（`_`, `□`, `X`, `❐`, `?`）を配列リテラルで持つ
6. ボタンのホバー/押下状態、押下中に外へ出たら解除される Windows の挙動を再現
7. アイコン取得（`_NET_WM_ICON` → サーバ側 Pixmap 変換、`WM_HINTS.icon_pixmap` フォールバック）
8. ウィンドウメニュー（`menu.c` の汎用ポップアップ実装。Phase 4 のスタートメニューと共用）
9. デスクトップ背景（ティール塗り + 任意で単一画像。画像デコーダは持たず、
   `xsetroot`/`feh` に任せる方針を明記）

**完了条件**: Win98 のスクリーンショットと並べて意匠差が無いこと（§10-3）。
比較用スクリプト `tools/pixdiff.sh` を用意し、参照画像との差分を出す。

---

## Phase 3 — EWMH 完全対応と現代アプリ互換

1. アトム一括取得（起動時に 1 往復でまとめて `InternAtom`）
2. ルートプロパティ群と、クライアントプロパティ群の読み書き
3. `_NET_WM_STATE` の全状態と、クライアントメッセージによる変更（`_NET_WM_STATE` 1/0/2=toggle）。
   **FULLSCREEN は最上層に固定せず、フォーカス時のみ**（SPEC §3.7.1）
4. `_NET_WM_WINDOW_TYPE` によるレイヤと装飾の決定（SPEC §5.2.2 の全 13 種の表を実装。
   ATOM 配列の走査と、認識できない ATOM の読み飛ばしを含む）
5. RandR: 1.5 の MONITOR 列挙（無ければ 1.2 の CRTC 列挙）、`RRScreenChangeNotify` に
   よるホットプラグ追従、モニタ跨ぎの最大化・全画面、`_NET_WM_FULLSCREEN_MONITORS`。
   **Xinerama 経路は書かない**（SPEC §5.3 で非対応と確定）
6. `_NET_WM_STRUT_PARTIAL` 収集 → `_NET_WORKAREA` 算出（複数パネル・複数モニタ対応）
7. `_NET_WM_SYNC_REQUEST`（SPEC §7.3 の状態機械を Phase 1 のペーシング機構に差し込む。
   **リクエストごとの `ChangeAlarm` による trigger 値更新**を忘れないこと（忘れると
   初回しか発火しない）。IDLE/WAITING/STALLED 遷移、250ms タイムアウト、
   ボタン解放時の target 再採番。**応答しないクライアントでリサイズが固まらないこと**を
   模擬クライアント（カウンタを持つが更新しない）で明示的にテストする）
8. `_NET_WM_PING`（無応答検出 →「応答なし」表示と強制終了ダイアログ）
9. `_MOTIF_WM_HINTS` と `_GTK_FRAME_EXTENTS`（CSD 対応、SPEC §7.2 の表に沿って
   visible rect 基準のジオメトリ計算まで実装。**最大化・全画面の遷移ごとに
   `_GTK_FRAME_EXTENTS` を読み直す**こと）
10. `_NET_WM_USER_TIME` によるフォーカススティール防止
11. Shape 拡張への追従（任意）

**完了条件**: §7.1 の全アプリで、全画面・ダイアログ・ポップアップ・リサイズが正常。
`xprop`/`wmctrl`/`xdotool` による自動チェックスクリプトが全項目 pass。

---

## Phase 4 — シェル（タスクバー・スタートメニュー・トレイ）

1. `taskbar.c` — パネルウィンドウ（`_NET_WM_WINDOW_TYPE_DOCK` + strut）、
   タスクボタン、押し込み状態、点滅、幅の自動調整とスクロール
2. 時計（`poll` のタイムアウトで分単位更新。タイマスレッドは持たない）
3. スタートボタン + スタートメニュー（縦バナー、階層、キーボード操作）
4. Alt+Tab スイッチャ（Win98 風の中央パネル、アイコン列 + 下部にタイトル）
5. デスクトップウィンドウ（右クリックメニュー、`_NET_SHOWING_DESKTOP`）
6. システムトレイ（`_NET_SYSTEM_TRAY_S0` セレクション、XEmbed、`_NET_SYSTEM_TRAY_OPCODE`）
   — Discord/Nextcloud/Telegram 等の常駐アプリで検証
7. オートハイド

**完了条件**: タスクバーだけで日常操作（起動・切替・最小化・復帰・終了）が完結する。
メモリ増分が 150 KB 未満に収まる。

---

## Phase 5 — 移植と最適化

1. OpenBSD でのビルド（`pkg_add libxcb` 系、BSD make との整合、`-I/usr/X11R6/include`）
2. `pledge(2)`：X 接続確立後に `stdio rpath wpath cpath unix proc exec` へ絞る。
   `exec` が必要なのは子プロセス起動のため。起動しない構成では更に絞る
3. `unveil(2)`：設定ディレクトリ・X ソケット・`/usr/X11R6` 等に限定
4. musl 静的リンク版のビルド（Alpine コンテナで検証、単一バイナリ配布）。
   **experimental 扱い**とし、正式サポートは動的リンクの core ビルド（SPEC §1.4）。
   バイナリサイズの実測目標は約 400 KB（`Private_Dirty` の改善手段ではない）
5. メモリ削減の実測ベース最適化
   - `bloaty` / `nm --size-sort` で .data/.bss を洗う
   - `valgrind --tool=massif` でヒープのピークと形を確認
   - xcb の内部バッファ設定と、リプライの取りこぼし（`xcb_flush` 忘れ）の点検
   - 起動後 `malloc_trim(0)`（glibc）
6. 起動時間の計測（目標: X 接続から最初の描画まで 20 ms 未満）

**完了条件**: OpenBSD 実機（または VM）で全機能が動作。§9.1 の指標 A が 1 MB 未満。

---

## Phase 6 — 仕上げ

1. 設定ファイルのパーサとバリデーション、`SIGHUP` 再読み込み、エラーメッセージ
2. 配色プリセット 5 種（Windows Standard / Rainy Day / Eggplant / Plum / High Contrast Black）
3. man ページ（`w98wm(1)`, `w98wm.conf(5)`）
4. README（スクリーンショット、ビルド手順、ディストリ別依存パッケージ、
   `.xinitrc` / セッションファイルの設置方法）
5. パッケージング: Arch PKGBUILD、Debian rules、OpenBSD port の雛形
6. 互換性マトリクス表（アプリ × 動作確認結果）を `docs/COMPAT.md` に記録
7. 8 時間ストレステスト、fuzz 的な異常プロパティ送信への耐性確認

---

## 検証戦略

| 種別 | 手段 |
| --- | --- |
| 単体 | 幾何計算（グラビティ、増分、strut、スナップ）を純関数として切り出し、X 無しで動く `tests/unit` を用意（TAP 出力、CI で実行） |
| 結合 | Xvfb + `xdotool`/`wmctrl`/`xprop` によるシナリオテスト（`tests/integration/*.sh`） |
| 目視 | Xephyr + 参照スクリーンショットとのピクセル差分（`tools/pixdiff.sh`） |
| メモリ | `tools/memcheck.sh`（CI で閾値強制）、`massif`、`xrestop`（サーバ側） |
| 安定性 | ランダムに起動/移動/リサイズ/終了を繰り返すストレススクリプト（8 時間） |
| 静的解析 | `-Wall -Wextra -Wpedantic -Werror`、`clang-analyzer`、`cppcheck`、CI で ASan/UBSan ビルド |

---

## 主要リスクと対策

| リスク | 影響 | 対策 |
| --- | --- | --- |
| メモリ 1 MB の定義次第で達成不能 | 目標未達に見える | §9.1 で指標を明確化し、承認時に合意しておく（未決事項 #1） |
| コアフォントの環境依存（`font-misc-misc` 等が未インストール、フォントパスが空） | 文字が出ない／起動失敗。**実装フェーズで最初につまずく箇所** | SPEC §4.5 に 9 段階のフォールバックチェーンと内蔵フォントを仕様として明記済み。Phase 2 の最初に実装し、フォントパスを空にした Xvfb での起動をテストに含める |
| 大きな iso10646 フォントへの `QueryFont` で数百 KB の一時リプライ | メモリ目標を単独で脅かす | SPEC §4.5.2 で 16bit フォントへの `QueryFont` を禁止し、`QueryTextExtents` に限定 |
| ドラッグのペーシング不足 | 低速リモート X でカクつく。後から直すとイベントループの構造に手が入る | SPEC §3.4.1 を Phase 1 の必須項目とし、遅延を入れた環境での検証を Phase 1 の完了条件に含める |
| `_NET_WM_SYNC_REQUEST` 待ちで無応答クライアントが操作を固める | リサイズ操作全体がハングして見える | 250ms タイムアウトで STALLED に落として続行（SPEC §7.3）。無応答クライアントを模したテストプログラムを用意 |
| `transient_for` の循環（壊れたアプリ・悪意ある入力） | スタッキング計算が無限ループしてハング | 深さ 16 段の制限と既訪問集合による循環検出（SPEC §3.7.2）。Phase 1 の単体テストに循環ケースを入れる |
| `_NET_WM_ICON` が巨大（256×256 で 1 枚 256 KB） | 読み込み時にメモリ目標を一時的に破る／サーバ側リソースが増え続ける | プロパティの分割読み込み、一辺 256 超の除外、クライアントあたり Pixmap 1 枚、変更時の即時解放（SPEC §4.4.1） |
| `_NET_SUPPORTED` に未完成のアトムを載せてしまう | クライアントが「対応済み」と誤認して不具合を起こす | アトムテーブルの `implemented` フラグから生成し、テストの無いアトムが載っていたら CI 失敗（SPEC §5.2.1） |
| GTK4 CSD アプリの見た目が Win98 にならない | 統一感の欠如 | 仕様として「CSD アプリは自前装飾のまま」と明記。`force_ssd` は best-effort |
| ICCCM/EWMH の実装漏れによる個別アプリの不具合 | 実用性の毀損 | Phase 3 完了時点で §7.1 の全アプリを通す。COMPAT.md に結果を残す |
| OpenBSD 実機の検証環境 | 移植品質 | Phase 5 で VM 検証。CI が無い分、手動チェックリストを整備 |
| Xephyr 等の検証ツールが本作業環境に無い | 開発中の目視確認が困難 | 依存のインストールを Phase 0 で行う。不可能な場合は Xvfb + スクリーンショット取得で代替し、逐次成果物を共有 |

---

## 進め方（レビューのしかた）

- ブランチ `claude/windows98-lightweight-wm-tissgc` に、フェーズ単位で push します。
- 各フェーズ完了時に、変更点の要約・スクリーンショット・メモリ実測値を報告します。
- 「Phase 1 まで作って一度見る」「Phase 2 まで一気に」など、区切り方のご希望があれば
  それに合わせます。既定では **Phase 1 → 一旦報告 → Phase 2 以降** の順で進めます。

---

*最終更新: 2026-08-08*
