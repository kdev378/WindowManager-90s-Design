# w98wm 仕様書

Windows 98 のルック＆フィールを再現しつつ、現代のアプリケーション（GTK3/4・Qt5/6・Electron・
Firefox・Chromium 等）が正常に動作する、超軽量な X11 リパレンティング型ウィンドウマネージャ。

| 項目 | 内容 |
| --- | --- |
| 名称 | **未定**（実装後に決定。仮称 `w98wm`、§11.1 の方法で後から変更可能にする） |
| 言語 | C99（フリースタンディング寄り、C++/STL 不使用） |
| 対象カーネル | Linux（glibc / musl）、OpenBSD |
| 対象ウィンドウシステム | X11 のみ（Wayland は将来の別バイナリとして検討、本仕様の対象外） |
| プロトコル準拠 | ICCCM 2.0（実装する節は §5.1）、EWMH 1.5、Motif WM Hints、XEmbed/System Tray（Phase 4） |
| メモリ目標 | `Private_Dirty` 1 MB 未満（§9.1 の指標 A。core ビルドのみ対象・承認済） |
| ライセンス | MIT（変更可） |

---

## 1. 設計方針

### 1.1 三つの制約の同時達成

本プロジェクトの難しさは、通常トレードオフになる 3 つを同時に満たす点にある。

1. **見た目の忠実さ** — Windows 98 の**視覚的外観**を、可能な限りピクセル精度で再現する
   （Windows の描画コードそのものの再現ではない。基準画像の定義は §4.0）。
2. **現代アプリの動作** — EWMH/ICCCM の実装漏れは、GTK のダイアログが画面外に出る、Qt の
   全画面が効かない、Electron がリサイズでちらつく等の形で即座に露見する。準拠は必須。
3. **1 MB 未満のメモリ** — Xlib・Xft・fontconfig・cairo・GLib を一切使わないことで達成する。

この 3 つ目の制約が、以下のすべての技術選択を規定する。

### 1.2 技術選択と根拠

| 領域 | 採用 | 不採用にしたもの | 根拠 |
| --- | --- | --- | --- |
| X クライアントライブラリ | **libxcb**（生の xcb + 自前ヘルパ） | Xlib | Xlib は接続時に約 200–400 KB のヒープを確保し、ロケール/XKB/リソースDBを抱え込む。xcb は薄い |
| 描画 | **xcb コアプロトコル**（PolyFillRectangle / PolyLine / ImageText16） | cairo, XRender, GTK | Win98 の意匠は矩形・1px 線・ベベル・水平グラデーションのみ。コア描画で完全再現できる |
| フォント | **X コアフォント**を既定（core ビルド）、`XFT=1` は別ビルドプロファイル（§1.4） | fontconfig + FreeType 必須構成 | コアフォントはグリフをサーバ側が持つためクライアント常駐量ゼロ。多言語表示の保証範囲は §4.5.5 |
| イベント待ち | **poll(2)** | epoll / kqueue | 2 OS 共通・fd 数が一桁のため性能差なし。移植層を薄くできる |
| 設定 | 起動時に読む key=value テキスト、パース後にバッファ解放 | GLib KeyFile, Lua, JSON | パーサ約 150 行、常駐コスト実質ゼロ |
| メモリ確保 | 固定スラブ + チャンク拡張（クライアント 64 個単位） | 汎用 malloc 依存の細切れ確保 | 断片化を避け、アイドル時のヒープを平坦に保つ |
| 依存 | §1.4 の表を参照 | xcb-util-wm(icccm/ewmh) | ICCCM/EWMH のプロパティ操作は自前実装（約 600 行）。依存とリンク時常駐を削減 |

### 1.3 非目標

- コンポジット（影・半透明・アニメーション）。外部コンポジタ（picom）との共存はするが、内蔵しない。
- タイル型レイアウト、スクリプト言語による拡張、テーマエンジン（配色のみ設定可能）。
- Wayland サポート。
- Win98 の**動作**の完全再現（あくまで見た目の再現 + 現代的な操作性の追加）。
- **RandR を持たない古い X サーバ**でのマルチモニタ動作（§5.3）。
- Xft ビルドにおけるメモリ目標の達成（§1.4）。

### 1.4 依存ライブラリとビルドプロファイル

| ライブラリ | 区分 | 用途 |
| --- | --- | --- |
| `libxcb` | **必須** | X プロトコル |
| `libxcb-randr` | **必須** | モニタ列挙・ホットプラグ（§5.3） |
| `libxcb-sync` | **必須** | `_NET_WM_SYNC_REQUEST` のカウンタとアラーム（§7.3） |
| `libxcb-keysyms` | **必須** | キーバインドの keysym ↔ keycode 変換 |
| `libxcb-shape` | 任意 | 非矩形ウィンドウの形状追従。無ければ矩形として扱う |
| `libxcb-xfixes` | 任意 | トレイの選択監視。無ければトレイ機能を無効化 |
| `libXft` + `libfontconfig` + `libfreetype` | **Xft ビルドのみ** | アンチエイリアス描画と多言語表示 |

**ビルドプロファイルは 2 つ**。混同を避けるため明確に分離する。

| プロファイル | 構成 | メモリ目標 | 位置づけ |
| --- | --- | --- | --- |
| **core**（既定） | libxcb 系のみ。コアフォント + 内蔵フォント | §9 の目標を**適用する** | **正式サポート**。動的リンク版が基準 |
| **xft**（`make XFT=1`） | 上記 + Xft/fontconfig/FreeType | §9 の目標は**適用しない**（実測 2 MB 前後を見込む） | 正式サポート（多言語表示や HiDPI を優先する場合の選択肢）。ただし「1 MB WM」を名乗るのは core のみ |

静的リンク（`make STATIC=1`、musl）は **experimental** とする。バイナリサイズ（実測目標
約 400 KB）と実行時の `Private_Dirty` は別の指標であり、静的リンクは後者をほとんど改善しない
（共有ライブラリのテキスト領域は元々 `Private_Dirty` に計上されない）。単一バイナリ配布の
利便性のための選択肢であって、メモリ目標の達成手段ではない。

---

## 2. アーキテクチャ

### 2.1 プロセス構成

単一プロセス・単一スレッド・イベント駆動。子プロセスを作るのはユーザがアプリを
起動したときのみで、`posix_spawn(3)` を使う（Linux / OpenBSD 両方にある）。

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
  stack.c       スタッキング（§3.7 のレイヤと transient チェーン）
  focus.c       フォーカスポリシー、フォーカス履歴、Alt+Tab
  menu.c        ウィンドウメニュー、スタートメニュー（共通のポップアップ実装）
  taskbar.c     タスクバー、スタートボタン、時計
  tray.c        システムトレイ（XEmbed, _NET_SYSTEM_TRAY_S0）※Phase 4
  config.c      設定パーサ
  util.c/.h     スラブアロケータ、文字列、ログ
  compat.h      Linux / OpenBSD 差分（pledge, strlcpy, poll ラッパ）
  brand.h       名称に関わる文字列の一元定義（§11.1）
```

### 2.2.1 シグナルとプロセス管理

- **SIGCHLD は `SIG_IGN` に設定する**。POSIX により、この設定下では終了した子は
  自動回収されゾンビにならない。w98wm は子の終了ステータスを一切必要としないため、
  これが最も単純で正しい。`waitpid` ループも self-pipe への SIGCHLD 通知も不要になる。
  - ただし `SIG_IGN` の設定は exec を跨いで**継承される**ため、起動したアプリ側の
    `wait()` が壊れる。`posix_spawn` の属性に `POSIX_SPAWN_SETSIGDEF` を指定して
    子側で SIGCHLD を `SIG_DFL` に戻すこと（これを忘れるとシェルスクリプト等が誤動作する）。
  - 同様に `posix_spawn` 属性で `POSIX_SPAWN_SETSIGMASK` を使い、子のシグナルマスクを空にする。
- **SIGTERM / SIGINT / SIGHUP のみ self-pipe** で受ける。
  - パイプは両端とも `O_NONBLOCK` + `O_CLOEXEC`。
  - ハンドラは async-signal-safe な `write(fd, &byte, 1)` のみを行い、
    **`EAGAIN`（パイプ満杯）は無視する**。この通知は「何かシグナルが来た」という
    エッジの合体で十分であり、取りこぼしても後続の 1 バイトで検知できる。
  - 実際に来たシグナルの種別は、ハンドラ内で `volatile sig_atomic_t` のフラグに
    立てて伝える（複数種が同時に来ても各フラグで区別できる）。
  - イベントループ側ではパイプを EOF/EAGAIN まで読み切ってからフラグを処理する。
- `SIGPIPE` は `SIG_IGN`。X 接続が切れた場合は `xcb_connection_has_error()` で検知して
  クリーンアップに入る。

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
    xcb_sync_counter_t sync_counter;  /* _NET_WM_SYNC_REQUEST */
    xcb_sync_alarm_t   sync_alarm;
    uint64_t     sync_target;     /* 最後に要求したカウンタ値 */
    uint8_t      sync_state;      /* IDLE / WAITING / STALLED (§7.3) */
    xcb_pixmap_t icon_pix, icon_mask;  /* 16x16、各 1 枚のみ (§4.4.1) */
    char         title[256];      /* UTF-8、NUL 終端 (§2.3.1) */
    uint8_t      title_len;
    struct client *next, *prev;   /* スタック順 */
    struct client *focus_next;    /* MRU 順 */
};
```

#### 2.3.1 タイトルはキャッシュする

タイトル文字列は構造体内に**キャッシュする**（256 B 固定、超過分は切り詰め）。

当初は「描画のたびに `_NET_WM_NAME` をサーバから読んで捨てる」設計にしていたが、これは誤り
だった。`GetProperty` はリプライを伴う同期リクエストであり、`Expose` のたびにラウンド
トリップが発生する。これは §3.4.1 で自ら定めた「**描画経路にリプライを伴う同期リクエストを
置かない**」という原則に正面から違反する。しかもタイトルの変更は `PropertyNotify` で
確実に検知できるため、そもそも毎回読む必要がない。

コストは 20 ウィンドウで 5 KB、200 ウィンドウでも 51 KB。§9 の目標に対して十分小さい。
**数 KB を節約するために X サーバとの同期を描画経路に持ち込むのは、トレードとして明確に
損である。** 更新は `PropertyNotify`（`_NET_WM_NAME` / `WM_NAME`）受信時のみ。

同じ原則により、`WM_CLASS`・`_NET_WM_PID` 等の低頻度参照プロパティは
キャッシュせず必要時に読むが、**いずれも描画経路には置かない**。

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

#### 3.4.1 ドラッグ中のフレームペーシング（必須）

ドラッグ中に `MotionNotify` 1 件ごとに `ConfigureWindow` を送る素朴な実装は、低速な
リモート X（実際によくある用途）や重量級クライアントで破綻する。以下を**実装上の必須要件**
とする。

1. **モーションの合体**：`MotionNotify` は毎回処理せず、イベントキューを空になるまで
   ドレインして**最後の 1 件だけ**を使う（`xcb_poll_for_queued_event` でループ）。
   ポインタは `PointerMotionHint` 付きで grab し、必要時に `QueryPointer` で現在位置を取る
   方式も併用できるようにする（サーバ→WM 方向のイベント量そのものを減らせる）。
2. **レート上限**：位置/サイズの反映は **16 ms（約 60 Hz）以上の間隔**を空ける。
   直前の反映からの経過時間が足りなければ、`poll(2)` のタイムアウトを残り時間に設定して
   次のループで反映する（タイマスレッドもタイマ fd も使わない）。
3. **未確定リクエストは 1 つまで（ドラッグ中のみ）**：リサイズ時は §7.3 の同期カウンタが
   返るまで次の `ConfigureWindow` を送らない。移動のみの場合はカウンタを使わず
   2. のレート上限で制御する。
4. **ドロップ時の確定**：ボタン解放時は 2. と 3. の抑制を解除し、最終ジオメトリを必ず
   1 回送る。同期カウンタとの整合は §7.3 の「ボタン解放時の扱い」で規定する
   （新しい target を採番して即送る）。

補足：`ConfigureWindow` はリプライを伴わない非同期リクエストであり、1 回ごとにサーバとの
往復（ラウンドトリップ）が発生するわけではない。ここでの実際のボトルネックは
(a) 送信バイト量とサーバ側のキュー滞留、(b) クライアント側の再描画コストの 2 つであり、
対策は上記のイベント合体とレート制御になる。ラウンドトリップを避ける設計（リプライを
待つ同期リクエストを描画経路に置かない）は別途原則として守る。

### 3.5 最大化・全画面

- 最大化は `_NET_WORKAREA`（strut を除いた領域）に対して行う。マルチモニタでは
  ウィンドウ中心が属するモニタの作業領域。
- 全画面（`_NET_WM_STATE_FULLSCREEN`）はモニタ全域、装飾なし、strut 無視。
  スタッキングは §3.7.1（フォーカス時のみ最上層）。
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

#### 3.7.1 レイヤ

EWMH の推奨スタッキング順に従い、下から順に以下の 6 層。

| # | レイヤ | 対象 |
| --- | --- | --- |
| 0 | `desktop` | `_NET_WM_WINDOW_TYPE_DESKTOP` |
| 1 | `below` | `_NET_WM_STATE_BELOW` |
| 2 | `normal` | 上記以外のすべて（**フォーカスされていない全画面ウィンドウを含む**） |
| 3 | `dock` | `_NET_WM_WINDOW_TYPE_DOCK`、`_NET_WM_STATE_ABOVE` |
| 4 | `above` | 一時的なポップアップ（`TOOLTIP` / `NOTIFICATION` / メニュー類） |
| 5 | `fullscreen` | **フォーカスされている**全画面ウィンドウのみ |

**全画面 ≠ ABOVE**。EWMH は最上層に置くのを「フォーカスされた全画面ウィンドウ」に
限定している。全画面ウィンドウがフォーカスを失ったら `normal` に落とすこと。これを
怠って全画面を常時最上層に置くと、パネル・ポップアップ・他アプリのダイアログが
全画面ウィンドウの裏に隠れて操作不能になる（この種の不具合の典型的な原因）。

#### 3.7.2 transient チェーン

レイヤと transient 関係が競合した場合、**レイヤ決定が先、transient は同一レイヤ内での
順序付け**とする。ただし親より低い層に沈まないよう、実効レイヤは以下で決める。

```
effective_layer(c) = max(layer_of(c), effective_layer(parent_of(c)))
```

- `transient_for` の子は、実効レイヤが同じ親のすぐ上に置く。
- チェーン（親 → transient → transient）を再帰的に辿る。
  **`transient_for` のループ（A→B→A や自己参照）は実在する**（壊れたアプリ、
  および悪意ある入力）。辿る深さを 16 段に制限し、既訪問集合で循環を検出して打ち切る。
  ここを無防備に書くと無限ループでハングする。
- `transient_for` がルートウィンドウを指す場合は、ICCCM に従い
  「そのウィンドウグループ全体に対する transient」として扱う（グループ内の最前面の上へ）。
- 親が最小化されたとき、その transient も併せて最小化する。逆に transient が
  `_NET_WM_STATE_MODAL` の場合、親をアクティブ化しようとしたら modal 側を前面に出して
  フォーカスを与える（親は入力を受け付けたままでよい。入力の遮断はアプリ側の責務）。
- `_NET_CLIENT_LIST_STACKING` は最終的な実スタック順で毎回更新する。

### 3.8 仮想デスクトップ

Win98 に仮想デスクトップは無いため既定 1 面。ただし EWMH の pager が正しく動くよう
`_NET_NUMBER_OF_DESKTOPS` 等は完全実装し、設定で 1–16 面まで増やせる（増やした場合のみ
タスクバー右にデスクトップ切替が現れる）。

---

## 4. 外観仕様（Windows 98 再現）

### 4.0 再現の基準

「ピクセル精度で再現する」の対象を明確にしておく。Win98 の描画は色深度・テーマ・
画面 DPI で変わるため、基準を固定しないと検証できない。

- **基準環境**：Windows 98 SE / 96 dpi / 32bit カラー / 配色「Windows スタンダード」/
  大きいフォント無効。この条件のスクリーンショットを `tests/reference/` に置く。
- **一致を要求する範囲**：フレーム・ベベル・キャプションバー・キャプションボタン・
  メニュー・タスクバーの**幾何と色**。これらは 1 ピクセルの差も許容しない。
- **一致を要求しない範囲**：フォントのグリフ形状（MS Sans Serif を同梱できないため）と、
  それに伴う文字位置の微差。

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

- 背景は左→右の水平グラデーション。**固定分割数ではなく、色が実際に変化する境界で
  分割する**。`#000080` → `#1084D0` の場合、R は 0→16、G は 0→132、B は 128→208 しか
  変化しないため、**幅が何ピクセルであっても異なる色は最大 133 段**にしかならない。
  この境界で `PolyFillRectangle` の矩形列を作れば、
  **真の per-pixel 補間と完全に同一の結果**が、幅に依存しない有界なリクエスト量で得られる
  （当初の「32 分割」は近似であり、§4.0 のピクセル一致要求を満たさないため撤回する）。
- アイコン：詳細は §4.4.1。
- タイトル文字：フォントの決定と失敗時の挙動は §4.5 に規定する。
  収まらない場合は末尾を `...` で省略。
- ボタン：右から `[X]` `[□]` `[_]`。`WM_NORMAL_HINTS` でリサイズ不可なら最大化ボタンを
  無効表示（グレイのエンボス）。`_NET_WM_ALLOWED_ACTIONS` に反映。
  ダイアログ（`transient_for` あり）は `[X]` と、`WM_HINTS` 次第で `[?]` のみ。
- ダブルクリックで最大化/復元、右クリックでウィンドウメニュー。

#### 4.4.1 アイコンの取得とサーバ側リソースの上限

クライアント側 RAM を減らす代わりに X サーバ側リソースを使う設計なので、**上限を仕様で
縛る**。`_NET_WM_ICON` は数 MB になり得る（256×256 の 1 枚だけで 256 KB）ため、無防備に
読むとクライアント側のメモリ目標も一時的に破る。

- **プロパティは分割して読む**。`GetProperty` の `long_offset` / `long_length` を使い、
  まず先頭の 2 ワード（幅・高さ）だけを読んでアイコン一覧を走査し、**採用する 1 枚の
  ピクセルデータだけ**を後から読む。プロパティ全体を一度にメモリへ載せない。
- **採用規則**：16×16 が存在すればそれを使う。無ければ 16×16 以上で最小のものを選び、
  最近傍で 16×16 に縮小する。**一辺 256 を超えるエントリは走査対象から除外**する。
  プロパティ全体が 4 MB を超える場合はアイコン無しとして扱う（防御的上限）。
- **保持は 1 クライアントあたり Pixmap 1 枚 + マスク 1 枚（16×16）のみ**。
  合計でも 1 枚あたり約 1.5 KB のサーバ側リソースに収まる。
- **アイコン変更時（`PropertyNotify`）は、新しい Pixmap を作ってから古い方を即座に
  `FreePixmap` する**。頻繁にアイコンを差し替えるアプリ（進捗表示など）でサーバ側の
  リソースが単調増加しないこと。クライアント破棄時も同様に解放する。
- 元の ARGB データはクライアント側に一切保持しない（変換用の一時バッファのみ、
  16×16×4 = 1 KB のスタック領域で足りる）。
- `_NET_WM_ICON` が無ければ `WM_HINTS.icon_pixmap` / `icon_mask`（サーバ側に既に存在する
  ため参照するだけ）、それも無ければ内蔵の既定アイコンを描く。
- M20 の測定時に `xrestop` でサーバ側リソースも記録し、ウィンドウ数に対して線形に
  収まっていることを確認する（§9.1）。

### 4.5 フォントの解決とフォールバック

コアフォントは近年インストールされていない環境が普通にあるため（`font-misc-misc` や
`xorg-fonts-*` が既定で入らないディストリ、フォントパスが空の X サーバ）、
**フォントの取得に失敗しても w98wm は必ず起動する**。フォント関連の失敗は fatal error に
しない。ただし「何が描けるか」の保証範囲は限定される（§4.5.5）。

#### 4.5.1 フォールバックチェーン

起動時に上から順に試し、最初に成功したものを採用する。採用結果は `-v` 時にログへ出力し、
`w98wm --print-font` でも確認できる。

| # | 候補 | 備考 |
| --- | --- | --- |
| 1 | 設定ファイルの `font`（指定時のみ） | 失敗時は警告のみ出して次へ（起動は継続） |
| 2 | `-*-tahoma-medium-r-normal--11-*-*-*-p-*-iso10646-1` | 存在すれば最も近い（Win98 の一部 UI で実際に使われた書体） |
| 3 | `-*-helvetica-medium-r-normal--11-*-*-*-p-*-iso10646-1` | 広く存在する近似。MS Sans Serif そのものではないが、11px ビットマップの字面としては実用上十分近い |
| 4 | `-*-helvetica-medium-r-normal--11-*-*-*-p-*-iso8859-1` | 同上（Latin-1 のみ） |
| 4b | `-*-lucida-medium-r-normal-sans-11-*-*-*-p-*-iso10646-1` | |
| 5 | `-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1` | `font-misc-misc`。等幅だが可読 |
| 6 | `-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1` | |
| 7 | `-*-*-medium-r-normal--1[0-4]-*-*-*-*-*-iso10646-1` | サイズだけ合わせた総当たり |
| 8 | `fixed` | 事実上すべての X サーバが持つ別名 |
| 9 | **内蔵ビットマップフォント** | §4.5.3。X サーバにフォントが 1 つも無い場合の最終手段 |

候補の照会には `ListFonts`（`xcb_list_fonts`）を使い、**マッチが 1 件以上あることを
確認してから** `OpenFont` する。

> **実装上の落とし穴**：xcb の `xcb_open_font()` は unchecked リクエストであり、存在しない
> フォント名を渡してもその場では失敗せず、後続の描画で `BadFont` が非同期に返る。必ず
> `xcb_open_font_checked()` + `xcb_request_check()` で同期的に確認すること。ここを誤ると
> 「起動はするが文字だけ出ない」「無関係な描画が壊れる」という追いにくい不具合になる。

#### 4.5.2 文字幅の計測（メモリ上の注意点）

`QueryFont` のリプライには**フォントの全文字の per-char メトリクス**が含まれる。
ISO10646 の大きなフォントでは 12 B × 数万文字 ＝ **数百 KB のリプライ**になり、
一時的とはいえ 1 MB 目標を単独で脅かす。したがって：

- `max_byte1 == 0`（＝ 256 文字以下の 8bit フォント）のときのみ `QueryFont` を使い、
  ASCII 範囲の幅表（96 × 2 B）と `max_bounds` を控えてリプライは即座に解放する。
- 16bit フォント（iso10646-1 等）では `QueryFont` を**呼ばない**。文字列幅は
  `QueryTextExtents`（`xcb_query_text_extents`）で必要時のみ取得する。
- 省略（`...`）位置の決定に二分探索で何度も往復しない。`max_bounds.character_width` から
  収まる文字数を見積もり、**追加の往復は最大 2 回**に制限する（1 回目で超過なら見積もりを
  縮めて 2 回目、それでも超過なら安全側に切り詰める）。
- タイトル描画は Expose・タイトル変更・フォーカス変化時のみで頻度が低いため、
  この往復コストは許容できる。ドラッグ等の高頻度経路には計測を置かない。

#### 4.5.3 内蔵フォント

候補 1–8 がすべて失敗した場合に使う、8×11 相当のプロポーショナルビットマップ
（ASCII + Latin-1、224 グリフ、幅表込みで約 3 KB を `.rodata` に持つ）。
起動時に 1bit の Pixmap アトラス 1 枚をサーバ側に作り、描画は `CopyPlane` で行う
（深度 1 → 深度 N の `CopyPlane` は GC の前景色/背景色で着色されるため、コアフォントと
同じ経路で扱える）。クライアント側の常駐増加は幅表のみ。

内蔵フォントは Latin-1 のみのため、この段階に落ちた場合、日本語等のタイトルは
豆腐（□）になる。その旨を起動時に一度だけ警告し、対処（フォントパッケージの導入、
または `XFT=1` ビルド）を案内する。

#### 4.5.4 CJK 補助フォント

日本語・中国語・韓国語のタイトルは、採用したコアフォントにグリフが無い場合がある
（例：`helvetica` を採用したケース）。Xlib の `XCreateFontSet` に相当する仕組みを
最小限で自前実装する：

- プライマリフォントに加えて **補助フォントを 1 つだけ** 開く
  （既定候補：`-*-*-medium-r-normal--14-*-*-*-c-*-jisx0208.1983-0`、
  `-misc-fixed-medium-r-normal-ja-*`、`-*-*-*-*-*-*-14-*-*-*-*-*-iso10646-1`）。
- 描画時に文字単位でプライマリのグリフ有無を判定し、無い文字だけ補助フォントで描く
  （`ImageText16` を font 切替を挟んで分割発行。ベースラインを揃える）。
- 補助フォントも見つからない場合は豆腐で描く（クラッシュも空白もさせない）。

#### 4.5.5 多言語表示の保証範囲（重要）

`iso10646-1` のフォントを開けることと、そのフォントに日本語のグリフが**存在すること**は
別問題である。core ビルドは CJK のグリフデータを内蔵しない（内蔵すれば数 MB になり、
§9 の目標と正面から衝突する）。したがって保証範囲を次のように限定する。

| 条件 | 保証 |
| --- | --- |
| どんな環境でも | **起動する。ASCII / Latin-1 のタイトルは必ず描画できる**（最終手段が内蔵フォント） |
| CJK グリフを持つコアフォントが存在する | 日本語等のタイトルを表示する（**best-effort**） |
| 存在しない | 該当文字は豆腐（□）で描画し、起動時に一度だけ警告を出す |
| 確実な多言語表示が必要 | **`XFT=1` ビルドを使う**（§1.4。メモリ目標は非適用） |

**「core ビルドで日本語タイトルを必ず表示する」とは謳わない。**
当初の記述はこの点を保証できないまま断定していたため、上記に改める。

### 4.6 ウィンドウメニュー（Alt+Space / タイトル左クリック）

`元のサイズに戻す(R) / 移動(M) / サイズ変更(S) / 最小化(N) / 最大化(X) / 閉じる(C) Alt+F4`
Win98 と同一の項目・順序・区切り位置。状態に応じてグレイアウト。

### 4.7 タスクバー（Phase 4）

- 画面下端（設定で上/左/右も可）、高さ 28px、上端に 1px の `hilight` ライン。
- `[スタート]` ボタン（押下でスタートメニュー）、クイック起動（任意）、タスクボタン領域、
  トレイ、時計（`HH:MM`、ホバーで日付）。
- タスクボタンは押し込み＝アクティブ、点滅＝`DEMANDS_ATTENTION`。数が多い場合は幅を縮め、
  下限（既定 44px）に達したらスクロール矢印を出す（Win98 と同じ挙動）。
- 自身に `_NET_WM_STRUT_PARTIAL` を設定し、他ウィンドウの最大化領域を正しく確保する。
- 自動的に隠す（オートハイド）対応。

### 4.8 スタートメニュー（Phase 4）

- 左端の縦バナー（`#808080` グラデ + 縦書き "w98wm"）を含む Win98 の見た目。
- 項目は設定ファイルで定義（`~/.config/w98wm/menu`）。XDG の `.desktop` を走査する
  実装は**行わない**（メモリ・複雑度・起動時間の観点。必要なら外部生成スクリプトを同梱）。
- 階層メニュー、キーボード操作（矢印/Enter/Esc/アクセラレータ）に対応。

---

## 5. 対応プロトコル

### 5.1 ICCCM

「ICCCM 2.0 準拠」と一言で書くとテストできないため、**実装する節を列挙する**。

| ICCCM の節 | 内容 | 備考 |
| --- | --- | --- |
| §4.1.2.3 | `WM_NORMAL_HINTS`（`WM_SIZE_HINTS`） | min/max/base/increment/aspect/gravity の全フィールド |
| §4.1.2.4 | `WM_HINTS` | `input`, `initial_state`, `icon_pixmap/mask`, `window_group`, `urgency` |
| §4.1.2.5 | `WM_CLASS` | タスクバーのグループ化に使用 |
| §4.1.2.6 | `WM_TRANSIENT_FOR` | ルート指定時のグループ transient を含む（§3.7.2） |
| §4.1.2.7 | `WM_PROTOCOLS` | `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING` |
| §4.1.2.8 | `WM_COLORMAP_WINDOWS` | フォーカス追従でのカラーマップ設定 |
| §4.1.3.1 | `WM_STATE` | Withdrawn / Normal / Iconic |
| §4.1.4 | 状態遷移とウィンドウのマップ／アンマップ | 自分の reparent 由来の `UnmapNotify` の除外を含む |
| §4.1.5 | ジオメトリの設定と `ConfigureNotify` | **無視した `ConfigureRequest` への synthetic `ConfigureNotify`**、全 10 種のグラビティ |
| §4.1.7 | 入力フォーカス（4 モデル） | No Input / Passive / Locally Active / Globally Active |
| §4.3 | マネージャセレクション `WM_S<n>` | `--replace` による置換と、他 WM への譲渡 |
| §2.8 | セレクションの `MANAGER` クライアントメッセージ | トレイ（Phase 4）でも使用 |

対応する自動テストを `tests/integration/icccm/` に節番号ごとに置く。

### 5.2 EWMH

#### 5.2.1 `_NET_SUPPORTED` に載せる条件

**`_NET_SUPPORTED` は「知っているアトム」ではなく「正しく実装し、テストが通っているアトム」
だけを列挙する。** クライアントはこのリストを見て挙動を変えるため、中途半端な実装を
載せると「対応しているはずなのに動かない」という最悪の状態になる。

実装上は、アトムテーブルの各エントリに `implemented` フラグを持たせ、
`_NET_SUPPORTED` の構築時にフラグが立ったものだけを書き出す。

```c
static struct atom_def atoms[] = {
    { "_NET_WM_STATE_FULLSCREEN", ATOM_SUPPORTED },   /* → _NET_SUPPORTED に載る */
    { "_NET_WM_STATE_SHADED",     ATOM_KNOWN },       /* 内部で使うが載せない */
    ...
};
```

フェーズの進行に応じてフラグを `ATOM_KNOWN` → `ATOM_SUPPORTED` に昇格させる。
CI では「`_NET_SUPPORTED` に載っている各アトムに対応する結合テストが存在すること」を
機械的に検査し、テストの無いアトムが載っていたら失敗させる。

以下は **v1.0 時点で `ATOM_SUPPORTED` にする予定**の一覧であり、実装完了までは載せない。

**ルート**: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`, `_NET_CLIENT_LIST`,
`_NET_CLIENT_LIST_STACKING`, `_NET_NUMBER_OF_DESKTOPS`, `_NET_CURRENT_DESKTOP`,
`_NET_DESKTOP_NAMES`, `_NET_DESKTOP_GEOMETRY`, `_NET_DESKTOP_VIEWPORT`, `_NET_WORKAREA`,
`_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, `_NET_MOVERESIZE_WINDOW`, `_NET_WM_MOVERESIZE`,
`_NET_RESTACK_WINDOW`, `_NET_REQUEST_FRAME_EXTENTS`, `_NET_SHOWING_DESKTOP`。

**クライアント**: `_NET_WM_NAME`, `_NET_WM_ICON_NAME`, `_NET_WM_VISIBLE_NAME`,
`_NET_WM_DESKTOP`, `_NET_WM_WINDOW_TYPE`(§5.2.2), `_NET_WM_STATE`(MODAL, STICKY,
MAXIMIZED_VERT/HORZ, SHADED, SKIP_TASKBAR, SKIP_PAGER, HIDDEN, FULLSCREEN, ABOVE, BELOW,
DEMANDS_ATTENTION, FOCUSED), `_NET_WM_ALLOWED_ACTIONS`, `_NET_WM_STRUT`,
`_NET_WM_STRUT_PARTIAL`, `_NET_WM_ICON`, `_NET_WM_PID`, `_NET_WM_USER_TIME`,
`_NET_WM_USER_TIME_WINDOW`, `_NET_FRAME_EXTENTS`, `_NET_WM_FULLSCREEN_MONITORS`,
`_NET_WM_SYNC_REQUEST`(+`_NET_WM_SYNC_REQUEST_COUNTER`), `_NET_WM_PING`,
`_NET_WM_BYPASS_COMPOSITOR`。

`_NET_WM_STATE_SHADED`（シェード＝タイトルバーだけに畳む）は Win98 に無い機能だが、
EWMH 準拠のため実装する（既定のキーバインドは割り当てない）。

#### 5.2.2 `_NET_WM_WINDOW_TYPE` の全 13 種と挙動

「全 13 種に対応」では実装が定まらないため、種別ごとの扱いを表で規定する。

| 種別 | 装飾 | レイヤ | フォーカス | タスクバー | 備考 |
| --- | --- | --- | --- | --- | --- |
| `_NET_WM_WINDOW_TYPE_DESKTOP` | 無 | `desktop` | 可 | 非表示 | 全画面固定、移動不可 |
| `_NET_WM_WINDOW_TYPE_DOCK` | 無 | `dock` | 不可 | 非表示 | strut を収集 |
| `_NET_WM_WINDOW_TYPE_TOOLBAR` | 小キャプション | `normal` | 可 | 非表示 | 切り離しツールバー |
| `_NET_WM_WINDOW_TYPE_MENU` | 小キャプション | `normal` | 可 | 非表示 | 切り離しメニュー |
| `_NET_WM_WINDOW_TYPE_UTILITY` | 小キャプション(13px) | `normal` | 可 | 非表示 | ツールパレット |
| `_NET_WM_WINDOW_TYPE_SPLASH` | 無 | `normal` | 不可 | 非表示 | 中央配置 |
| `_NET_WM_WINDOW_TYPE_DIALOG` | 有（最大化ボタン無） | `normal` | 可 | 表示 | 親の中央に配置 |
| `_NET_WM_WINDOW_TYPE_DROPDOWN_MENU` | 無 | `above` | 不可 | 非表示 | |
| `_NET_WM_WINDOW_TYPE_POPUP_MENU` | 無 | `above` | 不可 | 非表示 | |
| `_NET_WM_WINDOW_TYPE_TOOLTIP` | 無 | `above` | 不可 | 非表示 | |
| `_NET_WM_WINDOW_TYPE_NOTIFICATION` | 無 | `above` | 不可 | 非表示 | |
| `_NET_WM_WINDOW_TYPE_COMBO` | 無 | `above` | 不可 | 非表示 | |
| `_NET_WM_WINDOW_TYPE_DND` | 無 | `above` | 不可 | 非表示 | ドラッグ中のアイコン |
| `_NET_WM_WINDOW_TYPE_NORMAL` | 有 | `normal` | 可 | 表示 | 既定 |

**決定規則**（EWMH に従う）:

1. プロパティは ATOM の**配列**である。先頭から走査し、**w98wm が認識できる最初の
   ATOM を採用**する（認識できないものは読み飛ばす。これを忘れると独自 ATOM を先頭に
   置くアプリで誤動作する）。
2. 認識できる ATOM が 1 つも無い、またはプロパティ自体が存在しない場合：
   - `WM_TRANSIENT_FOR` が設定されていれば `DIALOG` とみなす。
   - そうでなければ `NORMAL` とみなす。
3. `override_redirect` のウィンドウは種別に関係なく管理対象外。
4. 種別による装飾の決定は、`_MOTIF_WM_HINTS` と CSD 判定（§7.2）より**優先度が低い**
   （それらが装飾なしを指示すれば、種別が `NORMAL` でも装飾しない）。

### 5.3 拡張

| 拡張 | 用途 | 必須 |
| --- | --- | --- |
| RandR 1.2+ | モニタ列挙、ホットプラグ、`_NET_WORKAREA` 更新 | **必須**。1.5 があれば MONITOR オブジェクトを使い、1.2–1.4 では CRTC を列挙する（差分は約 30 行）。RandR 自体が無い場合はスクリーン全体を単一モニタとして扱い、機能を縮退させて動作を継続する（起動失敗にはしない）。**Xinerama 経路は実装しない**（§1.3 の非目標。RandR を持たない X サーバでのマルチモニタは切り捨てる） |
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

GTK3/4 の一部アプリは自前でタイトルバーを描き、`_GTK_FRAME_EXTENTS`（影と不可視リサイズ
ボーダーの余白）を持つ。「CSD なら装飾を消す」だけでは足りず、**ジオメトリの基準が
ウィンドウ矩形から見かけの矩形へずれる**点まで規定しないと、最大化・全画面で必ず破綻する。

**用語**：ウィンドウの実矩形を `W`、`_GTK_FRAME_EXTENTS` を `(l, r, t, b)` とすると、
ユーザから見える矩形（visible rect）は `V = W − (l, r, t, b)` である。

| 局面 | 規定 |
| --- | --- |
| 装飾 | 付けない（フレームは作るがボーダー・キャプションを 0 にする） |
| ジオメトリの基準 | **`V` を基準に**配置・スナップ・エッジ吸着・タイル判定を行う。ユーザに `W` を意識させない |
| 最大化 | `V` が作業領域と一致するように `W = workarea + (l, r, t, b)` を設定する |
| 全画面 | 同様に `V` がモニタ全域と一致するようにする。GTK は全画面時に影を消して `_GTK_FRAME_EXTENTS` を **0 に更新する**ため、この値を先読みして計算してはならない |
| 状態遷移時 | 最大化・全画面・復元の**遷移のたびに `_GTK_FRAME_EXTENTS` を読み直す**。`PropertyNotify` でも追従し、変化したら現在の状態に応じてジオメトリを再計算する |
| `_NET_FRAME_EXTENTS` | CSD クライアントには `(0,0,0,0)` を設定する（WM 側の装飾は無いため） |
| 移動・リサイズ | `_NET_WM_MOVERESIZE` 経由の操作に対応すること（CSD アプリのタイトルバードラッグはこれを使う）。不可視ボーダー領域はクライアントが自前で処理するため、WM 側のボーダーヒットテストは行わない |

設定 `force_ssd=true` は、`_MOTIF_WM_HINTS` を無視して強制的に装飾を付けるモード。
GTK 側のタイトルバーは消せないため二重になる。**best-effort であり推奨しない**旨を
ドキュメントに明記し、`GTK_CSD=0` 環境変数の案内を併記する。

### 7.3 リサイズの滑らかさ（`_NET_WM_SYNC_REQUEST`）

これが無いと GTK/Qt アプリのリサイズが激しくちらつく。単一スレッド・`poll(2)` ベースで
破綻なく書けるよう、状態機械として明示的に規定する。

**プロトコルの前提**（ここを取り違えやすい）：`_NET_WM_SYNC_REQUEST_COUNTER` が
**カウンタ 1 個**の場合が基本プロトコルで、WM が指定した値までクライアントがカウンタを
進めることで「その設定を反映して描画し終えた」ことを伝える。値は単調増加する
**リクエスト ID** であり、偶奇に意味はない。
偶数／奇数でフレームの途中経過を表すのは**カウンタ 2 個**の拡張プロトコル
（extended frame sync、GTK と mutter が使う）であって別物である。
**本仕様は基本プロトコル（カウンタ 1 個）のみを実装する**。プロパティに 2 個入っていた
場合は 1 個目のみを使う。当初の記述は両者を混同していたため、ここで訂正する。

**準備**（クライアント登録時に 1 回）：

```c
target = 0;
alarm = xcb_sync_create_alarm(counter,
            XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE |
            XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_VALUE | XCB_SYNC_CA_EVENTS,
            /* value_type */ ABSOLUTE,
            /* test_type  */ POSITIVE_COMPARISON,
            /* value      */ 1,        /* 初期の trigger 値 */
            /* events     */ true);
```

**アラームは作りっぱなしにしてはならない。** `POSITIVE_COMPARISON` は
「カウンタ値 ≥ trigger 値」で発火する仕組みなので、リクエストのたびに
`xcb_sync_change_alarm()` で **trigger 値を新しい target に更新する**必要がある。
これを忘れると、最初の 1 回だけ発火してその後は永久に沈黙する（＝リサイズが常に
`STALLED` に落ちる）。

**リサイズ 1 コマの流れ**（クライアントごとに状態を持つ）：

```
IDLE ──(ドラッグでジオメトリ変化)──> target += 1
                                     ChangeAlarm(trigger = target)
                                     _NET_WM_SYNC_REQUEST(target) を送信
                                     ConfigureWindow 送信 → WAITING
WAITING ──(XSyncAlarmNotify: counter >= target)──> IDLE
                                     （保留ジオメトリがあれば即座に次を送る）
WAITING ──(次のモーション)──> 送らずに「保留ジオメトリ」だけ更新
WAITING ──(250ms 経過)──> STALLED
STALLED ──(以降)──> 同期を諦めて §3.4.1 の 16ms レート制御のみで送り続ける
                    （AlarmNotify が返ってきたら IDLE に復帰）
```

- `XSyncAlarmNotify` を受けたら、**必ず `target` と突き合わせる**。古い（すでに
  上書きされた）リクエストに対する通知が遅れて届くことがあるため、
  `counter >= target` を満たさない通知は捨てる。
- タイムアウトは `poll(2)` のタイムアウト値に反映する。**応答しないクライアントが
  リサイズ操作全体を固まらせてはならない**（この機構で最も壊れやすい点）。
- カウンタを持たないクライアント（多くの単純な X アプリ）は最初から `STALLED` と同じ扱い。
- カウンタ値は 64bit（`hi`/`lo` の 2 ワード）で扱う。32bit で扱って桁上がりを落とすと
  以後アラームが二度と発火しない。

**ボタン解放時の扱い**（§3.4.1-3 との衝突を解消する）：

「未確定リクエストは 1 つまで」は**ドラッグ中のペーシング規則**であって、操作の終端には
適用しない。ボタン解放時は `WAITING` であっても、**新しい target を採番して最終
ジオメトリを即座に送る**。カウンタ値は単調増加するため、新しい target に到達した時点で
古いリクエストも自動的に充足され、古い通知は上のルールで捨てられる。整合性は保たれる。

「未確定リクエストの完了を待ってから最終ジオメトリを送る」という設計も考えられるが、
その場合ユーザがボタンを離してから最大 250 ms、ウィンドウが最終サイズにならない。
操作の終端で待たせる理由はないため、**採番し直して即送る方を採用する**。

この状態機械は移動（リサイズを伴わない）には適用しない。移動時のカウンタ更新は不要。

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

**測定条件は 1 点ではなく 4 点取る**。単一条件だけだと、CI で閾値割れしたときに
「常駐の底が上がったのか／ウィンドウあたりの単価が増えたのか／リークしたのか」を
切り分けられないため。

| 条件 | 内容 | 判定基準 | この数値が読み取れること |
| --- | --- | --- | --- |
| M0 | 起動直後・ウィンドウ 0 枚・タスクバー無効 | `< 400 KB` | **常駐の底**（.data/.bss・再配置・xcb 接続・起動時確保） |
| M1 | 起動直後・ウィンドウ 0 枚・タスクバー/トレイ有効 | `< 550 KB` | シェル部分の固定費 |
| M20 | 20 ウィンドウ・タスクバー/トレイ有効 | `(M20 − M1) / 20 < 2 KB` **かつ** `M20 < 1024 KB` | ウィンドウあたりの限界費用 |
| M20h | M20 の状態で 1 時間稼働 + 500 回の開閉/移動/リサイズ | `M20h − M20 < 32 KB` | **リークの有無**（増分が単調なら断片化ではなくリーク） |

**M20 の判定は 2 条件の AND である。** 当初は「M20 < 1024 KB」とだけ書き、本文で
「2 KB/窓未満」と述べていたが、この 2 つは整合していなかった。M1 < 550 KB と
M20 < 1024 KB からは 23.7 KB/窓まで許容されてしまい、本文の主張を担保できない。

実質的に効くのは**限界費用の条件**の方である。M1 = 550 KB・2 KB/窓なら M20 は
約 590 KB に収まり、1024 KB には十分な余裕がある。この余裕は意図的なもので、
1 MB は**対外的に約束する上限**、限界費用の条件は**回帰をその場で検出するための
CI ゲート**という役割分担にする。窓あたりの単価が太った瞬間に、
まだ 1 MB に達していなくても CI が落ちる。

CI ではこの 4 点すべてを記録し、いずれかが基準を超えたらビルドを失敗させる。
これによりフェーズごとのコミットで「どこが太ったか」が履歴から追える。

参考として、Metacity は約 15–25 MB、Openbox は約 5–8 MB、i3 は約 4–6 MB（VmRSS）。
本プロジェクトは B の指標でも Openbox の 1/3 以下を狙う。

### 9.2 達成手段

1. core ビルドでは Xlib / fontconfig / FreeType / GLib / cairo を一切リンクしない（§1.4）。
2. クライアント情報はスラブ確保（64 個 × 約 448 B = 28 KB/チャンク。
   タイトルキャッシュ 256 B を含む。§2.3.1 の通りこれは意図した支出である）。
3. アイコンのピクセルデータをクライアント側に常駐させない（§4.4.1）。
   大きなプロパティは分割読み込みでピーク使用量も抑える。
4. 描画バッファを持たない（サーバ側 GC + 直接描画。ダブルバッファはタスクバーのみ、
   サーバ側 Pixmap 1 枚で行う）。
5. 16bit フォントへの `QueryFont` を禁止（§4.5.2）。数百 KB の一時リプライを避ける。
6. ビルド: `-Os -fno-plt -fno-asynchronous-unwind-tables -ffunction-sections
   -fdata-sections -Wl,--gc-sections`、`-fno-stack-protector` は**使わない**（安全性優先）。
7. 設定パース用バッファ・起動時の一時領域は起動完了後に解放し、`malloc_trim` を呼ぶ
   （glibc のみ）。

なお **musl 静的リンクはこの一覧に含めない**。静的リンクが減らすのは共有ライブラリの
ファイルバックドなページであり、これは元々 `Private_Dirty` に計上されない。
指標 A の改善手段ではないため、§1.4 の通り配布利便性のための experimental 扱いとする。

### 9.3 継続的な計測

CI（GitHub Actions）で Xvfb 上に §9.1 の M0 / M1 / M20 / M20h を再現し、
`smaps_rollup` の `Private_Dirty` を読んで**目標を超えたらビルドを失敗させる**
（M20h のみ実行時間の都合で毎コミットではなく nightly）。
数値は `docs/MEMORY.md` に履歴として追記し、フェーズごとの推移を残す。

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

## 11. 決定事項

以下は承認済みであり、実装はこの前提で進める。

| # | 論点 | 決定 | 状態 |
| --- | --- | --- | --- |
| 1 | メモリ目標の定義 | §9.1 の**指標 A（`Private_Dirty` < 1 MB）を主目標とする**。VmRSS は参考値に留める | **承認済** |
| 2 | 日本語タイトルの扱い | core ビルドでは **best-effort で可**（§4.5.5）。確実な表示は必須要件としない。ASCII / Latin-1 の描画のみを保証する。`XFT=1` ビルドを代替手段として提供する | **承認済** |
| 3 | タスクバー / スタートメニュー | **WM 本体に内蔵する**。`taskbar=false` で無効化でき、コンパイル時にも除外可能とする | **承認済** |
| 4 | MS Sans Serif 相当フォント | 同梱はライセンス上不可。§4.5.1 の候補列で近似する。より忠実な見た目が必要なら、フリーの互換ビットマップ BDF を別パッケージとして案内する | 決定（§4.5.1） |
| 5 | 名称 | **実装が動いてから決める**。それまでは仮称 `w98wm` を使う。ソース上の識別子・設定パス・アトム名の接頭辞は 1 箇所（`src/brand.h`）にまとめ、後から一括で変更できるようにしておく | 保留（意図的） |
| 6 | ライセンス | MIT を想定 | 未確定 |

### 11.1 名称変更に備えた実装上の約束

決定 #5 のため、以下を守る。後から名前を変えるのが `src/brand.h` の書き換えだけで済む状態を維持する。

- バイナリ名・設定ディレクトリ名・ログの接頭辞・`WM_CLASS`・`_NET_WM_NAME` に出す文字列は
  すべて `brand.h` のマクロ経由で参照し、ソース中にリテラルを直接書かない。
- 独自アトムを定義する場合も同じマクロを使う（例: `_W98WM_*`）。
- スタートメニューの縦バナーに描く文字列も同様。

---

*最終更新: 2026-08-08 / 対応する計画書: [PLAN.md](PLAN.md)*
