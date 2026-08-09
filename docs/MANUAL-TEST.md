# 手動検証の手引き

自動テスト（`make test`）で踏めない部分を実環境で確認するための手順書。
CLI しか無いマシンに GUI 環境を立てるところから書く。

Xvfb 上の自動テストで確認済みなのは
「プロトコルとして正しいか」まで。ここで確認したいのは
**実アプリが実際に期待どおり動くか**で、両者は別物。

---

## 0. arm64 で動くか

**動くはず。** ただし arm64 実機での実行は未確認（開発環境が x86_64 のため）。
アーキテクチャ依存になりうる箇所は以下のとおり潰してある:

| 懸念 | 確認したこと |
| --- | --- |
| `char` の符号（arm64 は既定で **unsigned**、x86 は signed） | `-funsigned-char` でビルドして全テスト通過。文字を扱う箇所は元から `(unsigned char)` にキャストしてある |
| エンディアン | aarch64 は既定でリトルエンディアン。x86_64 と同じなので差は出ない。X プロトコル側もバイトオーダを自分でネゴシエートする |
| 非境界整列アクセス | ARMv8 は通常のロード/ストアで非境界整列を許す。加えてプロパティのデータは xcb が整列済みで返す |
| コンパイラ依存 | gcc と clang の両方でビルドして警告 0 |
| ビルドフラグ | `Makefile` に `-march` / `-mtune` / SSE 系は一切無い |

なので、ビルドが通らない／挙動が違う場合は
**それ自体が報告に値するバグ**。遠慮なく投げてください。

```sh
make            # まずこれが通るか
make test       # 13 本すべて ok か
make memcheck   # M0/M1/M20 が PASS か（arm64 の実測値が知りたい）
```

`make memcheck` の数字は x86_64 とは違って当然なので、**出た値をそのまま**
教えてもらえると助かります（ページサイズが 4KB でない arm64 環境だと
`Private_Dirty` の粒度が変わるため、そこも見たい）。

---

## 1. GUI 環境を立てる

CLI しか無いマシンで X アプリを動かして**手元から見る**方法。
ディスプレイが刺さっていない前提（VPS や SBC への SSH）で書く。

### 推奨: Xvnc（X サーバ + VNC が 1 プロセス）

Xvfb は「描画するが誰にも見せない」ので、目視確認には VNC が要る。
`Xvfb` + `x11vnc` の 2 段でも動くが、`Xvnc`（TigerVNC）なら 1 個で済む。

```sh
# Debian / Ubuntu
sudo apt update
sudo apt install -y tigervnc-standalone-server xterm x11-utils x11-xserver-utils \
                    xdotool wmctrl imagemagick fonts-vlgothic

# Fedora / RHEL
sudo dnf install -y tigervnc-server xterm xorg-x11-utils xdotool wmctrl \
                    ImageMagick vlgothic-fonts

# Arch
sudo pacman -S --needed tigervnc xterm xorg-xprop xorg-xwininfo xdotool wmctrl \
                        imagemagick ttf-hanazono
```

`fonts-vlgothic`（日本語フォント）は入れておいてください。
**core ビルドの日本語タイトル表示は環境の X コアフォント次第**なので、
ここが検証対象そのものになります。

パスワードを設定して起動:

```sh
vncpasswd                      # 初回だけ。~/.vnc/passwd を作る

# WM を自分で起動したいので、セッションの自動起動は空にする
Xvnc :1 -geometry 1280x800 -depth 24 -rfbport 5901 \
        -rfbauth ~/.vnc/passwd -localhost &
```

`-localhost` を付けているので、手元のマシンから **SSH トンネル**で繋ぎます
（VNC を直接インターネットに晒さないため）:

```sh
# 手元のマシンで
ssh -L 5901:localhost:5901 ユーザ名@サーバ
```

あとは手元の VNC クライアント（macOS なら Finder の「サーバへ接続」で
`vnc://localhost:5901`、Windows なら TigerVNC Viewer / RealVNC）で
`localhost:5901` へ接続。

灰色または黒の何も無い画面が出れば成功です。ここに WM を載せます:

```sh
DISPLAY=:1 ./w98wm -v
```

`-v` を付けると採用されたフォントや管理したウィンドウがログに出ます。
**このログは不具合報告のときに一番役に立つので、必ず取っておいてください。**

```sh
DISPLAY=:1 ./w98wm -v 2>&1 | tee /tmp/w98wm.log
```

### 解像度を変えたいとき

`Xvnc` は起動時の `-geometry` で固定です。
マルチモニタや HiDPI（`scale=2`）を試すときは Xvnc を立て直してください。

```sh
# HiDPI 相当の検証
Xvnc :1 -geometry 2560x1440 -depth 24 ... &
mkdir -p ~/.config/w98wm
echo 'scale=2' >> ~/.config/w98wm/config
```

### ディスプレイが刺さっている場合（実機の X）

こちらのほうが「本物」の検証になります（RandR が本物になるため）。

```sh
sudo apt install -y xserver-xorg xinit xterm
echo 'exec /path/to/w98wm -v' > ~/.xinitrc
startx -- :0
```

Ctrl+Alt+F2 などで仮想端末を切り替えられるようにしてから試してください
（WM が固まったときの逃げ道。`pkill w98wm` で戻せます）。

---

## 2. 検証用アプリを入れる

SPEC §7.1 の一覧。**arm64 でパッケージがあるものだけ**挙げます。

```sh
sudo apt install -y \
  firefox-esr `# Gecko` \
  chromium `# Blink` \
  gedit `# GTK3` \
  gnome-text-editor `# GTK4` \
  vlc `# Qt5、全画面の検証にも使う` \
  libreoffice-writer `# 巨大な GTK/VCL アプリ` \
  openjdk-17-jdk `# Java/Swing。§3 参照` \
  pavucontrol `# GTK3 の小さいダイアログ` \
  xfce4-notifyd `# 通知ウィンドウ (_NET_WM_WINDOW_TYPE_NOTIFICATION)`
```

**Electron** は arm64 のパッケージが揃いにくいので、無理なら飛ばして構いません
（Chromium で Blink 側の経路はだいたい踏めます）。試すなら VSCodium が arm64 の
`.deb` を出しています。

**トレイアイコンを出す常駐アプリ**が欲しい場合、arm64 で入りやすいのは:

```sh
sudo apt install -y nextcloud-desktop   # Qt。トレイに常駐する
# または
sudo apt install -y blueman             # GTK。blueman-applet がトレイに出る
```

自動テスト（`120-tray-xembed.sh`）で XEmbed の埋め込み・`_XEMBED_INFO`・
取り外し・`kill -9` 後の生存までは確認済みなので、実アプリで見たいのは
**アイコンの絵が正しく出るか（背景が黒くならないか）** の 1 点だけです。
ここは実アプリの描き方に依存するので、自動テストでは原理的に踏めません。

---

## 3. Java/Swing のテストプログラム

「何を使えばいいか分からない」とのことだったので用意しました。
`tools/SwingTest.java` にあります。

```sh
cd /path/to/repo
DISPLAY=:1 java tools/SwingTest.java
```

`openjdk-17-jdk` 以降なら `javac` 不要で `.java` を直接実行できます。

**なぜ Swing だけ特別扱いするのか**: Swing は ICCCM の 4 つの入力モデルのうち
**Globally Active**（`WM_HINTS.input = False` かつ `WM_TAKE_FOCUS` あり）を使う
数少ない実装です。この場合 WM は `SetInputFocus` を呼んではならず、
`WM_TAKE_FOCUS` を送ってアプリに委ねる必要があります。
ここを間違えると **クリックしても文字が打てないウィンドウ**ができます。
他のツールキットでは踏めない経路なので、Swing が唯一の実地検証手段です。

---

## 4. 確認してほしいこと

上から順に、**壊れていたときの影響が大きい順**に並べています。
各項目の「これが崩れていたら」は、報告のときに何を見ればいいかの目安です。

### A. フォーカス（最重要）

> Swing の Globally Active は **`130-swing-globally-active.sh` で自動検証済み**
> （打鍵がアプリに届くところまで）。なので下の 1 行目は「実機でも同じか」の
> 確認であって、優先度は下げて構いません。

| 手順 | 期待 | これが崩れていたら |
| --- | --- | --- |
| `java tools/SwingTest.java` を開き、テキスト欄をクリックして入力 | 文字が入る。タイトルバーが青（アクティブ）になる | ICCCM §4.1.7 の Globally Active の扱い（`focus.c`） |
| Firefox とエディタを交互にクリック | クリックしたほうがアクティブになり、前のは灰色に | 同上 |
| Firefox でリンクを新しいウィンドウで開く | **新しい窓が勝手に前に出ずタスクバーのボタンが点滅**、または前に出る（どちらでもよいが挙動が一貫していること） | `_NET_WM_USER_TIME` によるフォーカススティール防止（SPEC §3.6） |

### B. リサイズ（GTK アプリ）

| 手順 | 期待 | これが崩れていたら |
| --- | --- | --- |
| gedit の枠を掴んで**ゆっくり**ドラッグ | 中身が枠にぴったり追従する | — |
| gedit の枠を掴んで**素早く振り回す** | 中身が遅れても、枠と中身がずれたまま固まらない。手を離せば必ず一致する | `_NET_WM_SYNC_REQUEST`（SPEC §7.3）。ここは実装の山場なので特に見てほしい |
| LibreOffice Writer で同じことをする | 同上 | 同上（重いアプリほど差が出る） |
| VLC で動画を開いてウィンドウを縮める | 縦横比のヒントに従って段階的に縮む | `WM_NORMAL_HINTS` の aspect / increment（ICCCM §4.1.2.3） |

### C. 全画面とダイアログ

| 手順 | 期待 |
| --- | --- |
| VLC で動画を全画面（F）にする | タスクバーも含めて画面全部が動画になる。Esc で戻る |
| 全画面のまま Alt+Tab で他の窓へ | **全画面の窓が最前面に居座らない**（SPEC §3.7.1。ここは意図的に他の WM と違う） |
| gedit で「名前を付けて保存」 | ダイアログが親の上に出る。親をクリックしてもダイアログの下に潜らない |
| Firefox で右クリックメニュー | メニューが装飾されずに出て、外をクリックすると消える |
| Firefox のブックマーク等のツールチップ | 装飾されない。フォーカスを奪わない |

### D. CSD（Electron / GTK4）

| 手順 | 期待 |
| --- | --- |
| `gnome-text-editor` を開く | **タイトルバーが二重にならない**（アプリ自身の描くヘッダバーと w98wm の枠が両方出ていたらバグ） |
| その窓を最大化 | 作業領域いっぱいになる。タスクバーに潜らない |

現状は `force.ssd` の設定で強制的に装飾を付けることもできます
（`~/.config/w98wm/config` に `force.ssd=true`）。両方の見え方を教えてください。

### E. タスクバーとトレイ

| 手順 | 期待 |
| --- | --- |
| 窓を 10 個くらい開く | ボタンが等分で縮む。44px より狭くはならない |
| ボタンの文字 | 日本語タイトルが化けない。入り切らないと `...` が付く |
| アクティブな窓のボタンを押す | 最小化される。もう一度押すと戻る |
| `nextcloud-desktop` などを起動 | **トレイアイコンの背景が黒くならない**（SPEC §4.8 の ARGB 問題） |
| 窓を最大化 | 下端がタスクバーの上で止まる。タイトルバーが画面外に出ない |

### F. 日本語表示

| 手順 | 期待 |
| --- | --- |
| `DISPLAY=:1 ./w98wm --print-font` | 採用されたフォント名が出る |
| 日本語のファイル名でエディタを開く | タイトルバーに日本語が出る |

**ここは best-effort です**（core ビルドは X コアフォントしか使わないため）。
豆腐（□）になる、CJK だけ描画されない、といった結果でも
**それが分かること自体が成果**なので、`--print-font` の出力と一緒に報告してください。
実用に耐えないようなら `make XFT=1` の xft ビルドを整備します。

### G. マルチモニタ（実機で 2 画面ある場合のみ）

| 手順 | 期待 |
| --- | --- |
| 窓をモニタ間で移動して最大化 | そのモニタいっぱいになる |
| ケーブルを抜く / `xrandr --output ... --off` | 消えたモニタにあった窓が残ったモニタへ回収される |
| タスクバー | プライマリモニタに残る |

---

## 5. 不具合を見つけたときに送ってほしいもの

```sh
# 1. WM のログ（-v 付きで起動していれば標準エラーに出ている）
#    "manage win=..." や X エラーの行が手がかりになります

# 2. 問題の窓のプロパティ一式
DISPLAY=:1 xprop -id $(DISPLAY=:1 xdotool selectwindow) > /tmp/win.txt
#    ↑ 実行後に問題の窓をクリック

# 3. ルートのプロパティ
DISPLAY=:1 xprop -root > /tmp/root.txt

# 4. スクリーンショット
DISPLAY=:1 import -window root /tmp/shot.png
```

この 4 点があれば、こちらで Xvfb 上に同じ状況を再現できることが多いです。

**WM が固まった / 画面が真っ暗になった場合**: 別の SSH セッションから
`pkill w98wm` で落とせます。X サーバ（Xvnc）は生き残るので、
アプリを開いたまま WM だけ差し替えられます。

---

## 6. 既知の未実装（報告しなくてよいもの）

以下は「まだ作っていない」だけなので、動かなくても仕様どおりです:

- スタートメニューの階層化（フラット 1 段のみ）
- タスクバーのオートハイド、ボタンのスクロール矢印、時計ホバーでの日付
- 最小化時のワイヤーフレーム・ズームアニメーション
- デスクトップの右クリックメニュー
- `Super` 単押しでのスタートメニュー（`Ctrl+Esc` は動きます）
- OpenBSD（Phase 5）
