# w98wm 開発計画

[仕様書 (SPEC.md)](SPEC.md) を実装するための工程計画。

---

## 0. 全体像

7 フェーズ。各フェーズ末に「動く状態」と検証手段を置き、メモリ計測を毎フェーズ実施する。
各フェーズは 1 コミット単位ではなく、レビュー可能な粒度で複数コミットに分割する。

| Phase | 内容 | 成果物 | 概算規模 |
| --- | --- | --- | --- |
| 0 | 土台 | ビルドシステム、CI、Xephyr 検証環境、計測スクリプト | 約 400 行 |
| 1 | コア WM | reparent / セーブセット / ICCCM / エラー処理 / フォーカス / 移動 / リサイズ / ペーシング / スタッキング / 設定 | 約 2,150 行 |
| 2 | Win98 外観 | 描画プリミティブ、フレーム、ボタン、フォント解決、ウィンドウメニュー | 約 1,750 行 |
| 3 | EWMH 完全対応 | 全 `_NET_*`、RandR、strut、全画面、sync request、Motif/CSD | 約 1,550 行 |
| 4 | シェル | タスクバー、スタートメニュー、Alt+Tab、デスクトップ、トレイ | 約 1,800 行 |
| 5 | 移植と最適化 | OpenBSD 対応、pledge/unveil、musl 静的、メモリ削減 | 約 500 行 |
| 6 | 仕上げ | ドキュメント、パッケージ、互換性検証、安定化 | 約 250 行 |

上表の合計は約 8,400 行だが、**これは楽観的な見積もりである**。ほぼ同一スコープ
（reparenting + フル EWMH + タスクバー + トレイ + メニュー）の JWM が約 3 万行、
IceWM や Openbox はさらに大きい。装飾を持たない軽量 WM（dwm 約 2 千行）とは
比較にならない規模になる。

**工程判断には 12,000〜15,000 行（C、ヘッダ含む）を見込むこと。**
上表の各フェーズの数字は「中核ロジックの規模」であって、エラー処理・境界条件・
プロパティの読み書きの定型コードを含んでいない。実測との差はフェーズごとに記録し、
乖離が大きければ以降の見積もりを補正する。テスト・スクリプトを含めた総量は
18,000 行程度になる可能性がある。

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
- `src/brand.h` — 名称に関わる文字列の一元定義（SPEC §11.1）。名称は実装後に決めるため、
  バイナリ名・設定パス・`WM_CLASS`・ログ接頭辞をここだけで差し替えられる状態を保つ。
  CI で「ソース中に仮称のリテラルが直接書かれていないこと」を `grep` で検査する
- `src/util.c` — スラブアロケータ、ログ（`-v` で stderr、既定は無出力）
- `tools/run-xephyr.sh` — Xephyr 上で w98wm を起動し、テスト用アプリを配置する
- `tools/memcheck.sh` — SPEC §9.1 の **M0 / M1 / M20 / M20h** の 4 条件を測定し表形式で出力。
  0 枚の測定（M0/M1）を Phase 0 の時点から取り始めることで、以降のフェーズで
  「常駐の底が上がった」のか「ウィンドウ単価が増えた」のかを毎回切り分けられるようにする
- `tools/compat-matrix.sh` — §7.1 のアプリを順に起動し、手動確認用チェックリストを出す
- `tools/check-supported.sh` — `_NET_SUPPORTED` に載っている各アトムに対応する結合テストが
  存在するかを機械的に検査する（SPEC §5.2.1。テストの無いアトムが載っていたら CI 失敗）
- GitHub Actions:
  - **Ubuntu（gcc / clang, glibc）** — ビルド + Xvfb 起動テスト + メモリ閾値チェック
  - **Alpine コンテナ（musl）** — musl ビルドはここで行う。Ubuntu 上の `musl-gcc` では
    **musl 向けにビルドされた libxcb 一式が無いため成立しない**（当初 Ubuntu の
    マトリクスに musl を入れていたのは誤り）
  - OpenBSD は CI に無いため、構文検査 + 手動検証手順を記載

**完了条件**: 何もしないバイナリが X に接続し、ルートウィンドウのイベントを受け、
`memcheck.sh` が数値を出す。

---

## Phase 1 — コア WM（ここで「使える」状態になる）

1. マネージャセレクション取得（`WM_S0`）、`SubstructureRedirect` の取得、
   既に WM が居る場合のエラー処理と `--replace`
2. 既存ウィンドウの adopt。走査条件は **`map_state == Viewable` または
   `WM_STATE` プロパティを持つもの**（後者を落とすと、前の WM が Iconic にした
   ウィンドウ＝unmapped + `WM_STATE=Iconic` を取りこぼして復帰不能になる）
3. `MapRequest` → 存在確認（checked `GetWindowAttributes`）→ フレーム生成 →
   **`ChangeSaveSet(INSERT)`** → reparent → map の一連。
   **セーブセットは最初に入れる**（SPEC §3.1.1）。これが無いと WM のクラッシュで
   全ウィンドウが道連れになる。通常操作では一切表面化しないため、後回しにすると
   実装漏れに気づけない
4. `ConfigureRequest` / `ConfigureNotify` / `UnmapNotify` / `DestroyNotify` /
   `PropertyNotify` / **エラー（`response_type == 0`）** のハンドリング。
   **Unmap の同期問題**（WM 起因の UnmapNotify を無視するカウンタ管理）を最初に
   正しく作る — ここを雑にすると後で必ず「ウィンドウが消える」バグになる。
   カウンタの対象は SPEC §3.8 の 3 つの発生源（reparent / 最小化 / デスクトップ切替）を
   最初から想定して設計する。エラー処理の方針は SPEC §2.2.2
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
11. 終了時のクリーンアップ（`SIGTERM` 時に全クライアントを root へ reparent し直し、
    `WM_STATE` を削除、セーブセットから `DELETE`）
12. **設定パーサ（SPEC §8）をここで作る**（当初 Phase 6 に置いていたが前倒し）。
    `focus.mode` や `taskbar=false` は Phase 1–4 の検証で切り替えたい値であり、
    Phase 6 まで全部ハードコードだとテストマトリクスが回せない。約 150 行と小さい

**この時点の見た目**: 装飾は単色の灰色矩形（プレースホルダ）。
**完了条件**:
- xterm と firefox を起動し、移動・リサイズ・フォーカス・閉じるが破綻なく動く
- **`kill -9` してもウィンドウが消えない**（セーブセットの検証。`SIGTERM` では
  graceful path が走ってしまい、セーブセットの不備を検出できない）
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
   `ListFontsWithInfo` による候補照会（FONTINFO の文字コード範囲を同時に取得）→
   `xcb_open_font_checked` + `request_check` による確実な検証 →
   CJK 補助フォント（範囲判定方式・SPEC §4.5.4）→ 内蔵ビットマップフォントを一続きで作る。
   **`PolyText16`**（UCS-2 変換、フォント切替アイテム込み。`ImageText16` は背景を
   塗り潰すため使わない。SPEC §4.5.4.1）、`QueryTextExtents` による幅計測（§4.5.2 の
   `QueryFont` 回避ルールを守る）、省略表示（`...`）。UTF-8 → UCS-2 変換は自前（約 40 行）。
   **計測結果は必ずキャッシュし、`Expose` 経路でラウンドトリップを発生させないこと**
   （SPEC §4.5.2.1）。ここを間違えると、下敷きウィンドウの再描画が集中する
   ドラッグ操作でリモート X が固まる
   **検証**：フォントパスを空にした X サーバ（`xset fp= rehash` / `Xvfb -fp ""`）でも
   起動して文字が出ることを、この段階のテストに含める
4. `deco.c` — フレーム全体の描画。`Expose` の矩形単位で必要な部分だけ再描画
5. キャプションボタン（16×14）と 1bit グリフ（`_`, `□`, `X`, `❐`）を配列リテラルで持つ
   （`?` は実装しない。SPEC §4.4 の通り X に伝達手段が無い）
6. ボタンのホバー/押下状態、押下中に外へ出たら解除される Windows の挙動を再現
7. アイコン取得（SPEC §4.4.1）。分割読み込み、差し替えレースの検出、
   `WM_HINTS.icon_pixmap` の深度別処理（深度 1 は `CopyPlane`）、Pixmap の即時解放
8. ウィンドウメニュー（`menu.c` の汎用ポップアップ実装。Phase 4 のスタートメニューと共用）
9. デスクトップ背景（ティール塗り + 任意で単一画像。画像デコーダは持たず、
   `xsetroot`/`feh` に任せる方針を明記）

**この段階でのボタンの扱い**: 最大化・最小化ボタンとウィンドウメニューの項目は
**描画と押下状態のみ**を実装する。実際の最大化・全画面のロジックは strut と
モニタ別作業領域に依存するため Phase 3。検証時の混乱を避けるため、Phase 2 時点では
ボタンを押しても何も起きなくてよい（ただし押下の見た目は正しく出ること）。

**完了条件**: Win98 のスクリーンショットと並べて意匠差が無いこと（§10-3）。
比較用スクリプト `tools/pixdiff.sh` を用意し、参照画像との差分を出す。
グラデーションは SPEC §4.4 の色境界分割で、per-pixel 補間と完全一致すること。

---

## Phase 3 — EWMH 完全対応と現代アプリ互換

1. アトム一括取得（起動時に 1 往復でまとめて `InternAtom`）
2. ルートプロパティ群と、クライアントプロパティ群の読み書き
3. `_NET_WM_STATE` の全状態と、クライアントメッセージによる変更（`_NET_WM_STATE` 1/0/2=toggle）。
   **FULLSCREEN は最上層に固定せず、フォーカスが自身か transient チェーンにある時のみ**
   （SPEC §3.7.1）。復元ジオメトリは最大化用と全画面用を分けて持つ（SPEC §3.5.1）
4. `_NET_WM_WINDOW_TYPE` によるレイヤと装飾の決定（SPEC §5.2.2 の全 13 種の表を実装。
   ATOM 配列の走査と、認識できない ATOM の読み飛ばしを含む）
5. RandR: 1.5 の MONITOR 列挙（無ければ 1.2 の CRTC 列挙）、`RRScreenChangeNotify` に
   よるホットプラグ追従、モニタ跨ぎの最大化・全画面、`_NET_WM_FULLSCREEN_MONITORS`。
   **Xinerama 経路は書かない**（SPEC §5.3 で非対応と確定）
6. `_NET_WM_STRUT_PARTIAL` 収集 → **モニタ別作業領域**の算出（SPEC §3.5.2 の適用規則。
   辺への接触判定と start/end 区間の交差判定を含む）→ ルートプロパティ
   `_NET_WORKAREA` の更新。この 2 つは別物なので混同しないこと
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
3. スタートボタン + スタートメニュー（縦バナー、階層、キーボード操作）。
   **`Super` 単押しでの起動は難所**：`Super+E` 等との共存には sync grab と
   `AllowEvents(ReplayKeyboard)` によるタップ判定が必要で、素朴に `GrabKey` すると
   修飾キーとしての `Super` が壊れる。ここは独立したタスクとして時間を取る
4. Alt+Tab スイッチャ（Win98 風の中央パネル、アイコン列 + 下部にタイトル）。
   候補に Iconic を含める（SPEC §6。`taskbar=false` 時の唯一の復帰手段）
4b. 最小化/復元のワイヤーフレーム・ズーム（SPEC §1.3。XOR ラバーバンド、
   設定 `animate.minimize`）
5. デスクトップウィンドウ（右クリックメニュー、`_NET_SHOWING_DESKTOP`）
6. システムトレイ（`_NET_SYSTEM_TRAY_S0` セレクション、XEmbed、`_NET_SYSTEM_TRAY_OPCODE`）。
   **`_NET_SYSTEM_TRAY_VISUAL` に 24bit ビジュアルを明示**し、背景を `face` 色で塗る
   （コンポジタ無しでの ARGB アイコン黒背景問題。SPEC §4.8）
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

**完了条件**: OpenBSD 実機（または VM）で全機能が動作すること。
**指標 A（`Private_Dirty`）の 1 MB 判定は Linux で行う** — OpenBSD には
`smaps_rollup` 相当が無く測定できないため（SPEC §9.1）。OpenBSD では
`ps -o rss` と `procmap(1)` の参考値を記録し、Linux の値と大きく乖離しないことを確認する。

---

## Phase 6 — 仕上げ

1. 設定のバリデーション強化、`SIGHUP` 再読み込み、エラーメッセージの整備
   （パーサ本体は Phase 1-12 で実装済み）
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
| セーブセット（`ChangeSaveSet`）の実装漏れ | WM のクラッシュで全ウィンドウが道連れになる。**通常操作では一切表面化しないため、テストしない限り気づけない** | Phase 1-3 で reparent と同時に実装し、完了条件を `kill -9` での検証にする（SPEC §3.1.1） |
| X エラー（`BadWindow` 等）を想定しない実装 | クライアントが消えるレースで落ちる／状態が壊れる | エラーイベント（`response_type == 0`）の処理を Phase 1-4 に含め、「クライアント宛は許容・自リソース宛はバグ」の二分を徹底（SPEC §2.2.2） |
| `transient_for` の循環（壊れたアプリ・悪意ある入力） | スタッキング計算が無限ループしてハング | 深さ 16 段の制限と既訪問集合による循環検出（SPEC §3.7.2）。Phase 1 の単体テストに循環ケースを入れる |
| 描画経路に残ったラウンドトリップ | リモート X でドラッグ時に画面全体が固まる。**過去 2 回のレビューで 2 回とも見落とされた類型** | タイトル計測結果のキャッシュ（SPEC §4.5.2.1）、CJK 判定の範囲方式（§4.5.4）。`Expose` ハンドラから同期リクエストを発行していないことをコードレビュー項目にする |
| `ImageText16` を使ってしまう | 文字の背後が単色の箱で塗られ、グラデーションのタイトルバーで §4.0 のピクセル一致を満たせない | `PolyText16` に統一（SPEC §4.5.4.1）。GC の font 属性が切替アイテムで変わる副作用にも注意 |
| 規模見積もりの過小 | 工程判断を誤る | JWM 約 3 万行との比較から 12,000〜15,000 行を見込む。フェーズごとに実測と見積もりの差を記録して補正する |
| `Super` 単押しと `Super+E` の共存 | 修飾キーとしての Super が壊れる／メニューが出ない | sync grab + `AllowEvents(ReplayKeyboard)` によるタップ判定。Phase 4 で独立したタスクとして時間を取る |
| トレイの ARGB アイコンが黒背景になる | 常駐アプリのアイコンが崩れる | `_NET_SYSTEM_TRAY_VISUAL` に 24bit を明示し背景を face 色で塗る（SPEC §4.8） |
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
