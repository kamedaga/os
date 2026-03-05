# main.zig Refactor: Boot Runtime Mode Switch

## 目的
- `enable_framebuffer_server_step1` / `enable_boot_log_console_process` / `enable_virtio_input_mouse` の多重 `if` を整理する。
- 起動モード分岐を 1 つの判定に寄せ、`main()` の読みやすさを上げる。

## 追加した要素
- `BootRuntimeMode` enum
  - `DiskUser`
  - `FramebufferIpc`
  - `BootLogConsole`
  - `MouseCompositor`
- `UserBootProcessSetup` struct
  - 起動時に使う page capability 群をまとめる返却型
- `determineBootRuntimeMode(mouse_driver_cfg)`
  - 実行時モード判定を共通化
- `activateThreadOrHalt(thread_index)`
  - thread activate 失敗処理を共通化
- `setupUserProcessesForMode(state, mode, mouse_driver_cfg)`
  - ページ確保・アドレス空間構築・thread activate までモード別に集約
- `setupFramebufferServerAccess(state, info)`
  - framebuffer capability 付与/検証/map/ログを共通化

## main 側の変更
- 旧: `if (enable_framebuffer_server_step1) { if (enable_boot_log_console_process) { ... } }`
- 新: `boot_runtime_mode` を一度決定し、`switch (boot_runtime_mode)` で処理。
- 対象:
  - プロセス初期化ブロック
  - `user page table ready` ログ出力分岐
  - ELF ロード分岐
  - `enter ring3` ログ分岐

## 挙動について
- 起動モード別の処理順序とロード対象は維持。
- `enable_virtio_input_mouse=true` かつ `mouse_driver_cfg=null` の場合は `BootLogConsole` モードへフォールバック。

## 検証
- `zig build test` 成功
- `zig build efi` 成功
