# w98wm

Windows 98 のルック＆フィールを再現する、X11 用の超軽量リパレンティング型ウィンドウマネージャ。
現代のアプリケーション（GTK3/4・Qt5/6・Electron・Firefox・Chromium）が正常に動くことを
前提条件としつつ、常駐メモリ 1 MB 未満を目指します。

対象: **Linux**（glibc / musl）と **OpenBSD**。ウィンドウシステムは **X11** のみ。

- 依存は libxcb のみ（Xlib / GTK / cairo / fontconfig を使いません）
- ICCCM 2.0 / EWMH 1.5 / Motif WM Hints に準拠
- タスクバー・スタートメニュー・システムトレイを内蔵（無効化可能）

## ドキュメント

| 文書 | 内容 |
| --- | --- |
| [docs/SPEC.md](docs/SPEC.md) | 仕様書（設計方針、外観・挙動の詳細、対応プロトコル、メモリ目標） |
| [docs/PLAN.md](docs/PLAN.md) | 開発計画（7 フェーズの工程、検証戦略、リスク） |

## 状態

**設計中** — 仕様書と計画書のレビュー待ち。実装はまだ開始していません。
