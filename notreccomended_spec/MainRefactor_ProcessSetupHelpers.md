# main.zig Refactor: Process Setup Helpers

## 目的
- `main.zig` の起動時分岐で肥大化していたプロセス初期化処理を関数分割する。
- 挙動を変えずに「読む単位」を小さくする。

## 追加した helper
- `allocPageForProcessOrHalt(...)`
  - `allocPageTo` + 失敗時ログ/停止を共通化
- `mapUserLinearRegionOrHalt(...)`
  - `mapUserLinearRegion` + 失敗時ログ/停止を共通化
- `setupMouseDriverProcess(state, cfg)`
  - Process0 作成
  - runtime/config/shared/vfb ページ確保
  - Process0 へのマップ
  - MMIO capability インストール
  - MouseDriver config ページ公開
- `setupBootLogConsoleProcess(state)`
  - Process1 作成
  - boot log page / stack page 確保
- `setupBootLogSenderProcess(state, shared_page)`
  - Process2 作成
  - shared page read-only capability + マップ
- `setupVirtualFramebufferSharing(state, vfb_page)`
  - Process0->Process1 cap 付与
  - VirtualFramebufferCapability 付与/検証
  - Process0/Process1 への shared map
  - 初期ゼロ化とログ出力

## main 側の変更
- `enable_framebuffer_server_step1 && enable_boot_log_console_process` 分岐の大きなインライン処理を helper 呼び出しへ置換。
- 既存 `createUserProcess(...)` と組み合わせ、起動分岐の見通しを改善。

## 検証
- `zig build test` 成功
- `zig build efi` 成功
