# main.zig Refactor: ELF Load Helpers

## 目的
- `main.zig` の ELF ロード処理（load失敗時停止、thread RIP/RSP設定、ログ出力）の重複を減らす。
- 起動分岐ごとの差分を「何をロードするか」に寄せて可読性を上げる。

## 追加した helper
- `loadUserElfIntoUserPageOrHalt(...)`
  - 単一ページ ELF ロード + 失敗時ログ/停止を共通化
- `loadUserElfIntoTwoPagesOrHalt(...)`
  - 2ページ ELF ロード + 失敗時ログ/停止を共通化
- `setThreadEntry(thread_index, entry, rsp: ?u64)`
  - `RIP` と必要時 `RSP` を設定
- `setThreadEntryIfReady(thread_index, entry, rsp)`
  - thread が ready な場合のみ `RIP/RSP` を設定
- `logElfLoadSummary(header, loaded)`
  - `base/entry/load_segments` のログを共通化

## main 側の変更
- 以下の起動経路でインライン実装を helper 呼び出しへ置換
  - MouseDriver
  - BootLogSender
  - Compositor
  - BootLogConsole
  - DrawClient
  - FramebufferServer
  - 単独 user ELF
- Boot log console 側のユーザーマップは既存 helper `mapUserLinearRegionOrHalt(...)` を利用する形に整理。

## 影響範囲
- 変更はリファクタリング（重複削減）が中心。
- ELF 配置先、entry 設定先 thread、既存ログの意味は維持。

## 検証
- `zig build test` 成功
- `zig build efi` 成功
