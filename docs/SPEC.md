# w98wm 仕様書

Windows 98 のルック＆フィールを再現しつつ、現代のアプリケーション（GTK3/4・Qt5/6・Electron・
Firefox・Chromium 等）が正常に動作する、超軽量な X11 リパレンティング型ウィンドウマネージャ。

| 項目 | 内容 |
| --- | --- |
| 名称（仮） | `w98wm`（バイナリ名 `w98wm`） |
| 言語 | C99（フリースタンディング寄り、C++/STL 不使用） |
| 対象カーネル | Linux（glibc / musl）、OpenBSD |
| 対象ウィンドウシステム | X11 のみ（Wayland は将来の別バイナリとして検討、本仕様の対象外） |
| プロトコル準拠 | ICCCM 2.0、EWMH 1.5、Motif WM Hints、XEmbed/System Tray（Phase 4） |
| メモリ目標 | 常駐ワーキングセット 1 MB 未満（定義は §9） |
| ライセンス | MIT（変更可） |

---

## 1. 設計方針

### 1.1 三つの制約の同時達成

本プロジェクトの難しさは、通常トレードオフになる 3 つを同時に満たす点にある。

1. **見た目の忠実さ** — Windows 98 の描画を「それっぽく」ではなくピクセル単位で再現する。
2. **現代アプリの動作** — EWMH/ICCCM の実装漏れは、GTK のダイアログが画面外に出る、Qt の
   全画面が効かない、Electron がリサイズでちらつく等の形で即座に露見する。準拠は必須。
3. **1 MB 未満のメモリ** — Xlib・Xft・fontconfig・cairo・GLib を一切使わないことで達成する。

この 3 つ目の制約が、以下のすべての技術選択を規定する。

### 1.2 技術選択と根拠

| 領域 | 採用 | 不採用にしたもの | 根拠 |
| --- | --- | --- | --- |
| X クライアントライブラリ | **libxcb**（生の xcb + 自前ヘルパ） | Xlib | Xlib は接続時に約 200–400 KB のヒープを確保し、ロケール/XKB/リソースDBを抱え込む。xcb は薄い |
| 描画 | **xcb コアプロトコル**（PolyFillRectangle / PolyLine / ImageText16） | cairo, XRender, GTK | Win98 の意匠は矩形・1px 線・ベベル・水平グラデーションのみ。コア描画で完全再現できる |
| フォント | **X コアフォント**（`-*-*-*-*-*-*-*-*-*-*-*-*-iso10646-1`）を既定、`XFT=1` で Xft を任意有効化 | fontconfig + FreeType 必須構成 | コアフォントはグリフをサーバ側が持つためクライアント常駐量ゼロ。ISO10646 エンコーディングを使うことで日本語タイトルも表示可能（§4.4） |
| イベント待ち | **poll(2)** | epoll / kqueue | 2 OS 共通・fd 数が一桁のため性能差なし。移植層を薄くできる |
| 設定 | 起動時に読む key=value テキスト、パース後にバッファ解放 | GLib KeyFile, Lua, JSON | パーサ約 150 行、常駐コスト実質ゼロ |
| メモリ確保 | 固定スラブ + チャンク拡張（クライアント 64 個単位） | 汎用 malloc 依存の細切れ確保 | 断片化を避け、アイドル時のヒープを平坦に保つ |
| 依存 | libxcb, libxcb-randr, libxcb-keysyms, （任意）libxcb-shape, libxcb-xfixes | xcb-util-wm(icccm/ewmh) | ICCCM/EWMH のプロパティ操作は自前実装（約 600 行）。依存とリンク時常駐を削減 |

### 1.3 非目標

- コンポジット（影・半透明・アニメーション）。外部コンポジタ（picom）との共存はするが、内蔵しない。
- タイル型レイアウト、スクリプト言語による拡張、テーマエンジン（配色のみ設定可能）。
- Wayland サポート。
- Win98 の**動作**の完全再現（あくまで見た目の再現 + 現代的な操作性の追加）。

---

## 2. アーキテクチャ

### 2.1 プロセス構成

単一プロセス・単一スレッド・イベント駆動。fork するのはユーザがアプリを起動したときのみ
（`posix_spawn` で二重 fork し、ゾンビを作らない）。

```
                +----------------------------------+
                |            w98wm (1 proc)        |
   X server <-->| xcb connection (fd)              |
                |   +--- event loop (poll)         |
   SIGCHLD ---->|   +--- self-pipe                 |
                |                                  |
                |  core/     : クライアント管理     |
                |  deco/     : 装飾描画            |
                |  proto/    : ICCCM/EWMH          |
                |  shell/    : タスクバー/メニュー  |
                +----------------------------------+
```

### 2.2 モジュール構成

```
src/
  main.c        起動・引数・シグナル・イベントループ
  wm.c/.h       スクリーン初期化、マネージャセレクション取得、既存ウィンドウの adopt
  client.c/.h   クライアント構造体、frame の生成/破棄、状態遷移
  event.c       X イベントディスパッチ
  icccm.c       WM_STATE, WM_NORMAL_HINTS, WM_PROTOCOLS, WM_TRANSIENT_FOR 等
  ewmh.c/.h     アトム定義、_NET_* の読み書き、クライアントメッセージ処理
  motif.c       _MOTIF_WM_HINTS（装飾抑止）、_GTK_FRAME_EXTENTS
  deco.c        フレーム描画（タイトルバー・ボーダー・ボタン）
  draw.c/.h     描画プリミティブ（bevel, gradient, fill, line, text）
  theme.c/.h    配色テーブル・メトリクス・グリフビットマップ
  font.c        テキスト計測/描画（コアフォント実装、任意で xft.c に切替）
  input.c       キーバインド、マウスバインド、grab 管理
  move.c        move / resize / スナップ / エッジ吸着 / Alt ドラッグ
  layout.c      RandR モニタ管理、作業領域(strut)計算、最大化/全画面
  stack.c       スタッキング（レイヤ: desktop < below < normal < dock < above < fullscreen）
  focus.c       フォーカスポリシー、フォーカス履歴、Alt+Tab
  menu.c        ウィンドウメニュー、スタートメニュー（共通のポップアップ実装）
  taskbar.c     タスクバー、スタートボタン、時計
  tray.c        システムトレイ（XEmbed, _NET_SYSTEM_TRAY_S0）※Phase 4
  config.c      設定パーサ
  util.c/.h     スラブアロケータ、文字列、ログ
  compat.h      Linux / OpenBSD 差分（pledge, strlcpy, poll ラッパ）
```

### 2.3 クライアント表現

```c
struct client {
    xcb_window_t frame;      /* 装飾ウィンドウ（親） */
    xcb_window_t win;        /* アプリのウィンドウ（子） */
    struct rect  geom;       /* クライアント領域のジオメトリ */
    struct rect  restore;    /* 最大化前の復元用 */
    struct size_hints hints; /* WM_NORMAL_HINTS の要約 */
    uint32_t     states;     /* _NET_WM_STATE のビットフラグ */
    uint32_t     flags;      /* 装飾有無, 入力可否, urgent, mapped 等 */
    uint8_t      type;       /* _NET_WM_WINDOW_TYPE */
    uint8_t      desktop;
    xcb_window_t transient_for, group;
    uint64_t     sync_counter; uint64_t sync_value;  /* _NET_WM_SYNC_REQUEST */
    struct client *next, *prev;   /* スタック順 */
    struct client *focus_next;    /* MRU 順 */
};
```

タイトル文字列は**保持しない**（構造体に持つと 1 クライアントあたり最大数百バイト）。
描画時に `_NET_WM_NAME` をサーバから読み、スタックバッファ（256 B）に受けて描画後に捨てる。
これはタイトル変更時と再描画時のみ発生するため、実測でコストは無視できる。

---

## 3. ウィンドウ管理仕様

### 3.1 フレーム構造

各クライアントは 1 枚の `frame` ウィンドウ（`InputOutput`, backing なし）に reparent される。
子ウィンドウは装飾分だけオフセットして配置。ボタン等の当たり判定はフレーム上の座標計算で行い、
サブウィンドウは作らない（ウィンドウ数を最小化するため）。

```
frame
 +-- borders (4px, 描画のみ)
 +-- caption (18px, 描画のみ)
 |     +-- icon(16x16) | title text | [_][□][X]  ← 座標計算による当たり判定
 +-- win (アプリのウィンドウ)
```

### 3.2 装飾の有無

以下の場合は装飾なし（フレームは作るが borders/caption を 0 にする、もしくは reparent しない）：

- `_MOTIF_WM_HINTS` で `MWM_DECOR_ALL` が無効、または decorations = 0
- `_NET_WM_WINDOW_TYPE` が `DESKTOP` / `DOCK` / `SPLASH` / `TOOLTIP` / `DND` /
  `DROPDOWN_MENU` / `POPUP_MENU` / `COMBO` / `NOTIFICATION`
- `override_redirect` が立っている（そもそも管理対象外）
- `_NET_WM_STATE_FULLSCREEN` 中
- `_GTK_FRAME_EXTENTS` を持つ CSD アプリ（GTK3/4）※§7.2

### 3.3 状態遷移（ICCCM 4.1.4）

`Withdrawn → Normal → Iconic` を `WM_STATE` プロパティで正しく通知する。
`Iconic`（最小化）は子をアンマップし frame もアンマップ、`_NET_WM_STATE_HIDDEN` を立てる。
タスクバーからの復帰、`_NET_ACTIVE_WINDOW` クライアントメッセージでの復帰に対応。

### 3.4 移動・リサイズ

- タイトルバードラッグで移動。既定は**ライブ移動**（Win98 の「ドラッグ中にウィンドウの内容を
  表示する」ON 相当）。`drag_outline=true` で Win95 風のラバーバンド枠に切替可能。
- ボーダー 8 方向ドラッグでリサイズ。`WM_NORMAL_HINTS` の
  `min/max size`, `resize increments`, `base size`, `aspect ratio`, `win_gravity` を厳密に適用
  （端末エミュレータの文字単位リサイズが正しく効くこと）。
- `Alt + 左ドラッグ` = 移動、`Alt + 右ドラッグ` = リサイズ（現代的な追加操作）。
- `_NET_WM_MOVERESIZE` クライアントメッセージ経由の移動/リサイズに対応（GTK CSD アプリの
  タイトルバードラッグがこれを使う）。
- **スナップ**：画面端・他ウィンドウ端への吸着（既定 8px）。Aero Snap 相当の
  「上端で最大化 / 左右端で半分」は既定 OFF（Win98 に無いため）、設定で有効化可能。

### 3.5 最大化・全画面

- 最大化は `_NET_WORKAREA`（strut を除いた領域）に対して行う。マルチモニタでは
  ウィンドウ中心が属するモニタの作業領域。
- 全画面（`_NET_WM_STATE_FULLSCREEN`）はモニタ全域、装飾なし、strut 無視、最上位レイヤ。
  `_NET_WM_FULLSCREEN_MONITORS` に対応（複数モニタ跨ぎ）。
- 動画プレイヤ・ブラウザの全画面が確実に効くことは受け入れ条件（§10）。

### 3.6 フォーカス

- 既定は **click-to-focus**、`focus_follows_mouse=true` で sloppy focus。
- `WM_HINTS.input` と `WM_TAKE_FOCUS` を組み合わせた ICCCM 準拠のフォーカス付与
  （Globally Active クライアント＝Java/Qt の一部が正しく動くために必須）。
- `_NET_ACTIVE_WINDOW` の source indication を尊重（pager からの要求は許可、アプリからの
  自己アクティブ化は `_NET_WM_USER_TIME` で判定し、古い場合は
  `_NET_WM_STATE_DEMANDS_ATTENTION`（＝タスクバーボタン点滅）に落とす）。
- MRU 順のフォーカス履歴を保持し、ウィンドウ破棄時に適切な次のウィンドウへ移す。

### 3.7 スタッキング

レイヤ（下→上）: `desktop` < `below` < `normal` < `dock` < `above` < `fullscreen`。
`transient_for` の子は常に親より上。`_NET_CLIENT_LIST_STACKING` を毎回更新。

### 3.8 仮想デスクトップ

Win98 に仮想デスクトップは無いため既定 1 面。ただし EWMH の pager が正しく動くよう
`_NET_NUMBER_OF_DESKTOPS` 等は完全実装し、設定で 1–16 面まで増やせる（増やした場合のみ
タスクバー右にデスクトップ切替が現れる）。

---

## 4. 外観仕様（Windows 98 再現）

### 4.1 配色（既定スキーム "Windows Standard"）

| 役割 | 色 | 用途 |
| --- | --- | --- |
| `face` | `#C0C0C0` | 3D フェース（フレーム、ボタン、メニュー、タスクバー） |
| `hilight` | `#FFFFFF` | ベベル内側の明部 |
| `light` | `#DFDFDF` | ベベル外側の明部 |
| `shadow` | `#808080` | ベベル内側の暗部 |
| `dkshadow` | `#0A0A0A` | ベベル外側の暗部（実機は純黒だが 98 の実描画に合わせ `#0A0A0A`） |
| `active_title_l` / `_r` | `#000080` → `#1084D0` | アクティブタイトルバーの水平グラデーション |
| `inactive_title_l` / `_r` | `#808080` → `#B5B5B5` | 非アクティブタイトルバー |
| `title_text` | `#FFFFFF` | アクティブタイトル文字 |
| `inactive_title_text` | `#C0C0C0` | 非アクティブタイトル文字 |
| `desktop` | `#008080` | デスクトップ背景（ティール） |
| `menu_bg` / `menu_text` | `#C0C0C0` / `#000000` | メニュー |
| `highlight` / `highlight_text` | `#000080` / `#FFFFFF` | メニュー選択 |
| `window_bg` / `window_text` | `#FFFFFF` / `#000000` | 入力欄など |
| `disabled_text` | `#808080` | 無効項目（`#FFFFFF` の 1px オフセット付き） |

全色は設定ファイルで上書き可能。プリセットとして "Windows Standard" のほか
Win98 同梱スキーム（Rainy Day, Eggplant, Plum, High Contrast Black 等）を数点同梱する。

### 4.2 メトリクス（96 dpi 基準）

| 名称 | 値 | 備考 |
| --- | --- | --- |
| サイジングボーダー幅 | 4 px | 内訳：外側ベベル 1px + 内側ベベル 1px + フェース 2px |
| 固定ボーダー幅（リサイズ不可窓） | 3 px | ダイアログ相当 |
| タイトルバー高 | 18 px | `SM_CYCAPTION` |
| ツールウィンドウのタイトル高 | 13 px | `SM_CYSMCAPTION`（`UTILITY` タイプ） |
| キャプションボタン | 16 × 14 px | 上下 2px マージン |
| ボタン間隔 | 0 px（閉じるの左のみ 2px） | Win98 の実配置 |
| タイトルアイコン | 16 × 16 px | 左端から 2px |
| タスクバー高 | 28 px | ボタン高 22px |
| メニュー項目高 | 18 px（セパレータ 7px） | |

`--dpi` / 設定で 2 倍（HiDPI）スケールに対応。整数倍のみ（ピクセルアートの破綻を避けるため）。

### 4.3 ベベル描画

Windows の `DrawEdge` 相当を 4 種類実装する。左上と右下に 1px 線を引くだけの処理。

| 種別 | 外側 TL / BR | 内側 TL / BR | 用途 |
| --- | --- | --- | --- |
| `EDGE_RAISED` | `light` / `dkshadow` | `hilight` / `shadow` | ウィンドウフレーム、ボタン通常 |
| `EDGE_SUNKEN` | `shadow` / `hilight` | `dkshadow` / `light` | 入力欄、へこみ |
| `EDGE_PRESSED` | `shadow` / `face` | `dkshadow` / `face` | ボタン押下（内容を 1px 右下へずらす） |
| `EDGE_BUMP`(1px) | `hilight` / `shadow` | — | メニューセパレータ、グリップ |

ウィンドウフレームの実際のピクセル並び（外→内）：

```
1px: TL=#DFDFDF  BR=#0A0A0A     ← 外側ベベル
1px: TL=#FFFFFF  BR=#808080     ← 内側ベベル
2px: #C0C0C0                    ← フェース
--- ここからキャプション/クライアント領域 ---
```

### 4.4 タイトルバー

- 背景は左→右の水平グラデーション。`PolyFillRectangle` で 32 分割の矩形列として描く
  （実機も 8bit 環境ではディザ、TrueColor では滑らか。32 分割で視覚的に等価かつ 1 回の
  リクエストにまとめられる）。
- アイコン：`_NET_WM_ICON`（ARGB）から 16×16 を選び、無ければ `WM_HINTS.icon_pixmap`、
  それも無ければ既定のアプリアイコンを描く。`_NET_WM_ICON` はサーバ側 Pixmap に一度だけ
  変換して保持し、クライアント側にはピクセルデータを常駐させない。
- タイトル文字：フォント既定 `-*-helvetica-medium-r-normal--11-*-iso10646-1`
  （MS Sans Serif 相当のビットマップフォント。同梱の互換 BDF を用意する案は §11 参照）。
  収まらない場合は末尾を `...` で省略。
- ボタン：右から `[X]` `[□]` `[_]`。`WM_NORMAL_HINTS` でリサイズ不可なら最大化ボタンを
  無効表示（グレイのエンボス）。`_NET_WM_ALLOWED_ACTIONS` に反映。
  ダイアログ（`transient_for` あり）は `[X]` と、`WM_HINTS` 次第で `[?]` のみ。
- ダブルクリックで最大化/復元、右クリックでウィンドウメニュー。

### 4.5 ウィンドウメニュー（Alt+Space / タイトル左クリック）

`元のサイズに戻す(R) / 移動(M) / サイズ変更(S) / 最小化(N) / 最大化(X) / 閉じる(C) Alt+F4`
Win98 と同一の項目・順序・区切り位置。状態に応じてグレイアウト。

### 4.6 タスクバー（Phase 4）

- 画面下端（設定で上/左/右も可）、高さ 28px、上端に 1px の `hilight` ライン。
- `[スタート]` ボタン（押下でスタートメニュー）、クイック起動（任意）、タスクボタン領域、
  トレイ、時計（`HH:MM`、ホバーで日付）。
- タスクボタンは押し込み＝アクティブ、点滅＝`DEMANDS_ATTENTION`。数が多い場合は幅を縮め、
  下限（既定 44px）に達したらスクロール矢印を出す（Win98 と同じ挙動）。
- 自身に `_NET_WM_STRUT_PARTIAL` を設定し、他ウィンドウの最大化領域を正しく確保する。
- 自動的に隠す（オートハイド）対応。

### 4.7 スタートメニュー（Phase 4）

- 左端の縦バナー（`#808080` グラデ + 縦書き "w98wm"）を含む Win98 の見た目。
- 項目は設定ファイルで定義（`~/.config/w98wm/menu`）。XDG の `.desktop` を走査する
  実装は**行わない**（メモリ・複雑度・起動時間の観点。必要なら外部生成スクリプトを同梱）。
- 階層メニュー、キーボード操作（矢印/Enter/Esc/アクセラレータ）に対応。

---

## 5. 対応プロトコル

### 5.1 ICCCM

- マネージャセレクション `WM_S<n>` の取得と `--replace` による置換、他 WM への譲渡。
- `WM_STATE`, `WM_PROTOCOLS`(`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING`),
  `WM_NORMAL_HINTS`, `WM_HINTS`, `WM_CLASS`, `WM_NAME`, `WM_ICON_NAME`,
  `WM_TRANSIENT_FOR`, `WM_COLORMAP_WINDOWS`, `WM_CHANGE_STATE`。
- `ConfigureRequest` への正しい応答（無視した場合の synthetic `ConfigureNotify` 送出）。
- ウィンドウグラビティの適用（`StaticGravity` を含む全 10 種）。

### 5.2 EWMH（`_NET_SUPPORTED` に列挙するもの）

**ルート**: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`, `_NET_CLIENT_LIST`,
`_NET_CLIENT_LIST_STACKING`, `_NET_NUMBER_OF_DESKTOPS`, `_NET_CURRENT_DESKTOP`,
`_NET_DESKTOP_NAMES`, `_NET_DESKTOP_GEOMETRY`, `_NET_DESKTOP_VIEWPORT`, `_NET_WORKAREA`,
`_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, `_NET_MOVERESIZE_WINDOW`, `_NET_WM_MOVERESIZE`,
`_NET_RESTACK_WINDOW`, `_NET_REQUEST_FRAME_EXTENTS`, `_NET_SHOWING_DESKTOP`。

**クライアント**: `_NET_WM_NAME`, `_NET_WM_ICON_NAME`, `_NET_WM_VISIBLE_NAME`,
`_NET_WM_DESKTOP`, `_NET_WM_WINDOW_TYPE`(全 13 種), `_NET_WM_STATE`(MODAL, STICKY,
MAXIMIZED_VERT/HORZ, SHADED, SKIP_TASKBAR, SKIP_PAGER, HIDDEN, FULLSCREEN, ABOVE, BELOW,
DEMANDS_ATTENTION, FOCUSED), `_NET_WM_ALLOWED_ACTIONS`, `_NET_WM_STRUT`,
`_NET_WM_STRUT_PARTIAL`, `_NET_WM_ICON`, `_NET_WM_PID`, `_NET_WM_USER_TIME`,
`_NET_WM_USER_TIME_WINDOW`, `_NET_FRAME_EXTENTS`, `_NET_WM_FULLSCREEN_MONITORS`,
`_NET_WM_SYNC_REQUEST`(+`_NET_WM_SYNC_REQUEST_COUNTER`), `_NET_WM_PING`,
`_NET_WM_BYPASS_COMPOSITOR`。

`_NET_WM_STATE_SHADED`（シェード＝タイトルバーだけに畳む）は Win98 に無い機能だが、
EWMH 準拠のため実装する（既定のキーバインドは割り当てない）。

### 5.3 拡張

| 拡張 | 用途 | 必須 |
| --- | --- | --- |
| RandR 1.5 | モニタ列挙、ホットプラグ、`_NET_WORKAREA` 更新 | 必須（無い場合は Xinerama、それも無ければ単一画面にフォールバック） |
| XSync | `_NET_WM_SYNC_REQUEST`（GTK/Qt のリサイズちらつき防止） | 必須 |
| Shape | 非矩形ウィンドウのフレーム形状追従 | 任意 |
| XFixes | ポインタ/選択の監視（トレイ用） | 任意 |
| XKB | キーマップ変更の追従 | 任意（無ければ `MappingNotify` で再 grab） |

---

## 6. 操作仕様（既定のキー/マウス）

| 操作 | 動作 |
| --- | --- |
| `Alt+Tab` / `Alt+Shift+Tab` | Win98 風タスクスイッチャ（アイコン列＋タイトル表示の中央パネル） |
| `Alt+Esc` | スイッチャなしで次のウィンドウへ |
| `Alt+F4` | 閉じる（`WM_DELETE_WINDOW`、応答が無ければ `_NET_WM_PING` タイムアウト後に強制終了確認） |
| `Alt+Space` | ウィンドウメニュー |
| `Alt+F7` / `Alt+F8` | キーボードによる移動 / リサイズ（矢印キー、Enter 確定、Esc 取消） |
| `Ctrl+Alt+←/→` | デスクトップ切替（多面時のみ） |
| `Ctrl+Esc` | スタートメニュー |
| `Super` | スタートメニュー |
| `Super+D` | デスクトップの表示 / 復元（`_NET_SHOWING_DESKTOP`） |
| `Alt+左ドラッグ` / `Alt+右ドラッグ` | 移動 / リサイズ |
| ボーダードラッグ / タイトルドラッグ | リサイズ / 移動 |
| タイトルダブルクリック | 最大化 / 復元 |
| デスクトップ右クリック | ルートメニュー |

すべて設定ファイルで再定義可能。任意コマンドの起動バインドも定義できる。

---

## 7. 現代アプリ互換性

「Win98 風だが実用にならない」を避けるため、以下を明示的に受け入れ条件とする。

### 7.1 検証対象アプリ

Firefox / Chromium（サイト全画面・PiP・ポップアップ）、GTK3（gedit, nautilus）、
GTK4（gnome-text-editor）、Qt5/Qt6（qterminal, VLC）、Electron（VS Code）、
Java/Swing（globally-active フォーカス）、SDL2 ゲーム（全画面切替）、
xterm/alacritty（リサイズ増分）、mpv（全画面）、ダイアログ多用アプリ（GIMP）。

### 7.2 GTK クライアントサイド装飾（CSD）

GTK3/4 の一部アプリは自前でタイトルバーを描き、`_GTK_FRAME_EXTENTS`（影の余白）を持つ。

- これらは装飾を付けず、`_GTK_FRAME_EXTENTS` 分を差し引いた「見かけの矩形」を基準に
  配置・最大化・スナップを行う（差し引かないとウィンドウが端からずれて見える）。
- CSD ウィンドウでも `_NET_WM_MOVERESIZE` で移動・リサイズできること。
- 設定 `force_ssd=true` で `_GTK_CSD_DISABLE` 相当（`GTK_CSD=0` 環境変数の案内）と
  強制装飾を試みるモードを用意する（完全な強制は不可能なため best-effort と明記）。

### 7.3 リサイズの滑らかさ

`_NET_WM_SYNC_REQUEST` を実装し、リサイズ中はカウンタの応答を待ってから次の
`ConfigureWindow` を送る。これが無いと GTK/Qt アプリのリサイズが激しくちらつく。

### 7.4 その他

- `_NET_WM_PING` による無応答検出。無応答ウィンドウはタイトルに `(応答なし)` を付け、
  閉じる操作で「プログラムの終了」ダイアログ（Win98 風）を出す。
- 起動通知（`_NET_STARTUP_ID`）：受理してタイムアウトさせるだけの最小実装
  （砂時計カーソルの表示に利用）。

---

## 8. 設定

`~/.config/w98wm/config`（無ければ `/etc/w98wm/config`）。

```ini
# 外観
theme            = standard        # standard|rainy|eggplant|plum|hicontrast|custom
color.face       = #C0C0C0
color.desktop    = #008080
font             = -*-helvetica-medium-r-normal--11-*-*-*-*-*-iso10646-1
scale            = 1               # 1 or 2

# 挙動
focus.mode       = click           # click|sloppy
focus.raise      = true
drag.outline     = false           # true で Win95 風ラバーバンド
snap.distance    = 8
desktops         = 1
taskbar          = true
taskbar.position = bottom
taskbar.autohide = false
tray             = true

# キーバインド
bind = Alt+F4          close
bind = Alt+Tab         switch-next
bind = Super+E         exec  pcmanfm
bind = Ctrl+Alt+T      exec  xterm
```

パースは起動時のみ。`SIGHUP` で再読み込み。パーサは行単位・スタックバッファ 512B で処理し、
結果は固定サイズ構造体＋キーバインド配列（最大 128 個 = 約 2 KB）に格納する。

---

## 9. メモリ目標と測定方法

### 9.1 目標の定義（重要）

「1 MB 未満」は測り方で結果が変わるため、次のように定義する。

| 指標 | 目標 | 説明 |
| --- | --- | --- |
| **A. プライベートダーティ（主目標）** | **< 1024 KB** | Linux: `/proc/self/smaps_rollup` の `Private_Dirty`。ヒープ・スタック・.data/.bss・再配置分。プロセスが実際に「消費している」メモリ |
| B. VmRSS（参考値） | < 3 MB 目安 | 共有ライブラリのテキスト（libc/libxcb）を含む。他プロセスと共有されるページも含むため、本目標の主指標にはしない |
| C. X サーバ側リソース | 記録する | ウィンドウ数・Pixmap（アイコンキャッシュ）の合計。`xrestop` で計測 |

条件：**20 ウィンドウを開き、タスクバー・トレイ有効、1 時間稼働後**。
アイドル時にヒープが単調増加しないこと（リーク無し）を同時に確認する。

参考として、Metacity は約 15–25 MB、Openbox は約 5–8 MB、i3 は約 4–6 MB（VmRSS）。
本プロジェクトは B の指標でも Openbox の 1/3 以下を狙う。

### 9.2 達成手段

1. Xlib / fontconfig / FreeType / GLib / cairo を一切リンクしない。
2. クライアント情報はスラブ確保（64 個 × 約 160 B = 10 KB/チャンク）。
3. タイトル文字列・アイコンピクセルをクライアント側に常駐させない（§2.3, §4.4）。
4. 描画バッファを持たない（サーバ側 GC + 直接描画。ダブルバッファはタスクバーのみ、
   サーバ側 Pixmap 1 枚で行う）。
5. ビルド: `-Os -fno-plt -fno-asynchronous-unwind-tables -ffunction-sections
   -fdata-sections -Wl,--gc-sections`、`-fno-stack-protector` は**使わない**（安全性優先）。
6. Linux では musl による完全静的リンク版もビルド可能にする（配布用に約 200 KB の
   単一バイナリ、動的リンクの共有ページが無い分 A と B がほぼ一致する）。
7. 設定パース用バッファ・起動時の一時領域は起動完了後に解放し、`malloc_trim` を呼ぶ
   （glibc のみ）。

### 9.3 継続的な計測

CI（GitHub Actions）で Xvfb 上に 20 ウィンドウを開き、`smaps_rollup` を読んで
**1024 KB を超えたらビルドを失敗させる**。数値は履歴としてリポジトリに記録する。

---

## 10. 受け入れ条件

以下がすべて満たされた時点で v1.0 とする。

1. §7.1 の全アプリが起動・移動・リサイズ・最大化・全画面・閉じるを正常に実行できる。
2. `xprop -root _NET_SUPPORTED` が §5.2 の全アトムを列挙し、`wmctrl -m` が正しく応答する。
3. Win98 実機スクリーンショットとの比較で、フレーム・タイトルバー・ボタンの
   ピクセル差分が意匠部分でゼロ（フォントとグラデーション量子化を除く）。
4. §9.1 の指標 A が 1024 KB 未満。
5. Linux（glibc/musl）と OpenBSD の両方でビルド・動作する。
6. 8 時間の連続稼働でクラッシュ・リークが無い（valgrind + ストレススクリプト）。
7. WM がクラッシュしても管理下のウィンドウが失われない（フレーム破棄時に子を
   ルートへ reparent し直す。ICCCM 準拠のクリーンアップ）。

---

## 11. 未決事項（承認時に方針をいただきたい点）

| # | 論点 | 提案 |
| --- | --- | --- |
| 1 | メモリ目標の定義 | §9.1 の指標 A（プライベートダーティ < 1 MB）を主目標としたい。VmRSS で 1 MB 未満は、libc を静的リンクしても達成が極めて厳しく、動的リンクでは実質不可能なため |
| 2 | フォント方式 | 既定は X コアフォント（常駐 0、日本語も ISO10646 フォント経由で表示可）。`XFT=1` でアンチエイリアス版もビルド可能に。ただし Xft 有効時はメモリ目標 A を約 1.5–2 MB に緩和する必要あり |
| 3 | MS Sans Serif 相当フォント | 同梱はライセンス上不可。既定は `-*-helvetica-*-11-*-iso10646-1`（多くの環境に存在）とし、より近い見た目のフリー BDF（例: `Tahoma` 代替）の導入は任意パッケージとして案内 |
| 4 | タスクバー/スタートメニュー | WM 本体に内蔵（Win98 の体験に不可欠、かつ外部パネルは軒並み数十 MB 消費するため）。`taskbar=false` で完全に無効化でき、その分のコードはコンパイル時にも除外可能にする |
| 5 | 名称 | `w98wm`。商標的な懸念を避けるなら `chicagowm` / `nine8` 等も可 |
| 6 | ライセンス | MIT を想定 |

---

*最終更新: 2026-08-08 / 対応する計画書: [PLAN.md](PLAN.md)*
