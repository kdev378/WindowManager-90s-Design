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

## 状態

**Phase 1 完了** — コア WM が動作します。装飾描画は Phase 2 から。

```
make          # ビルド (依存: libxcb, xcb-randr, xcb-sync, xcb-keysyms)
make test     # Xvfb 上で結合テスト
make memcheck # メモリ実測 (SPEC §9.1)
```

現時点で動くもの: ウィンドウの管理・reparent、ICCCM/EWMH の主要プロパティ、
フォーカス（ICCCM の 4 入力モデル）、移動・リサイズ（レート制御付き）、
スタッキング、キーバインド、複数デスクトップ、マルチモニタ (RandR)。

| 指標 | 実測 | 目標 |
| --- | --- | --- |
| Private_Dirty (0 窓) | 232 KB | < 400 KB |
| Private_Dirty (20 窓) | 252 KB | < 1024 KB |
| 窓あたりの限界費用 | 約 1.2 KB | < 2 KB |

詳細と読み方は [docs/MEMORY.md](docs/MEMORY.md)。装飾・タスクバーが未実装の
段階の数字であり、最終的な達成を保証するものではありません。

名称は実装が動いてから決めるため、現在の `w98wm` は仮称です（SPEC §11.1）。
