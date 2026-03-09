# Framebuffer Server Step1 (No IPC)

## 目的
- FramebufferServer プロセスを最小構成で起動する。
- IPC はまだ実装せず、まずは「画面塗りつぶし」ができることを確認する。

## 今回の実装
- UEFI `GraphicsOutput` (GOP) を `ExitBootServices` 前に取得。
- 2MiB のユーザー PT ウィンドウに収まる GOP モードを選択。
- `FramebufferCapability` を追加し、fb 物理情報を型で保持。
- Process0 (FramebufferServer) のユーザー空間に fb を linear map。
- ring3 コードで `rep stosd` により framebuffer 全体を単色塗りつぶし。

## 変更ファイル
- `kernel/src/kernel.zig`
  - `FramebufferCapability` 追加。
- `kernel/src/main.zig`
  - GOP 取得/モード選択 (`acquireFramebufferInfo`)。
  - Framebuffer を Process0 へ map (`mapUserLinearRegion`)。
  - 起動経路を FramebufferServer 塗りつぶしデモへ切替。
- `kernel/src/user_programs.zig`
  - `installFramebufferFillCode` 追加。
  - ring3 で `mov rdi, fb_va; mov eax, color; mov rcx, pixels; rep stosd` を実行。

## ブート時ログの要点
- `framebuffer server ready`
- `mode=<id>`
- `fb_paddr=<phys>`
- `fb_size=<bytes>`
- `fb_va=<user virtual>`
- `fb_resolution=<WxH>`
- `enter ring3 with iretq (framebuffer server fill)`

## 制約 (Step1)
- IPC なし。
- Framebuffer map は「単一 Process0」固定。
- 2MiB ウィンドウ内に収まるモードのみ対象。
- capability テーブルとの厳密な統合 (ページ単位追跡) は Step2 以降。

## 次ステップ案
- `FramebufferCap` を CNode 管理に統合。
- FramebufferServer への IPC (map request / fill / blit) を導入。
- 複数プロセスからの委譲・revoke と PTE 同期を統合。
