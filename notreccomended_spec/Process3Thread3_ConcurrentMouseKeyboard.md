# Process3/Thread3 Concurrent Mouse+Keyboard

## 目的
- mouse / keyboard を同時常駐できるようにする。
- `MouseCompositor` 経路で `BootLogSender(Process2)` と `KeyboardDriver(Process3)` を並行起動する。

## 主要変更
- `kernel/src/kernel.zig`
  - `PrincipalId.Process3` を追加
  - process principal 判定を `Process0..Process3` に拡張
  - principal table 数を 5 (`Process0..3 + Device0`) に拡張
  - `initPhase1` / `initFromDetectedRegions` の cap/endpoint/mailbox 初期化に `Process3` を追加
- `kernel/src/capability.zig`
  - `processIndex` / `principalFromProcessIndex` に `Process3` を追加
  - capability dump に `Process3` を追加
- `kernel/src/main.zig`
  - `user_process_count = 4`, `user_thread_count = 4`
  - `thread_contexts` に `Thread3(owner=Process3)` を追加
  - `processIndex` / `principalLabel` / `threadLabel` に Process3/Thread3 を追加
  - `syscall_grant_cap` の宛先に `Process3` を追加
  - `KeyboardDriverProcess` を `Process3/Thread3` に移動
  - 起動セットアップ結果に `process3_user_page/process3_user_stack_page` を追加
  - `MouseCompositor` モードで keyboard が見つかれば `Process3` 作成・`Thread3` activate
  - ELF ロードで keyboard を `Thread3` にマップ
  - 起動ログ/CR3ログに `Process3/Thread3` を追加
- `kernel/src/virtio_probe.zig`
  - `probeMouseModern` / `probeKeyboardModern` 分離のまま利用
  - keyboard probe を mouse 成功時でも実行し、同時常駐準備
- `run.sh`
  - `-device virtio-keyboard-pci` を追加して同時接続を有効化

## ログ確認
- 受領ログでは mouse デバイスのみ検出:
  - `virtio-probe: classify rel_xy=0 abs_xy=1 key_a=0 btn_left=1`
  - `KeyboardDriverProcess disabled (modern input not found)`
- これは `virtio-tablet` のみの構成で期待どおり。
- `virtio-keyboard-pci` を追加した構成で keyboard 分類ログが出れば `Process3/Thread3` 常駐に進む。

## 検証
- `zig build test` 成功
- `zig build efi` 成功
