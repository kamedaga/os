# Virtio Input Queue Non-Contiguous Fix

## 目的
- `MouseDriver` / `KeyboardDriver` が queue 用 2 ページを「物理連続」と仮定して停止する問題を解消する。
- `allocPage` が返すページが非連続でも virtqueue を正しく構成できるようにする。

## 問題
- 旧実装では `queue_paddr1 == queue_paddr0 + 4096` を必須としていた。
- 非連続時に `queue pages non-contiguous` で無限停止していた。
- 実機/ブート時のページ割当は非連続が普通に起きるため、この前提は不適切。

## 修正内容
- `kernel/user_programs/mouse_driver.zig`
  - `queueRegionPhys(queue_paddr0, queue_paddr1, offset)` を追加。
  - `common_queue_used` に設定する物理アドレスを helper 経由に変更。
  - event buffer descriptor の `addr` 計算を helper 経由に変更。
  - 非連続チェックで停止するコードを削除。
- `kernel/user_programs/keyboard_driver.zig`
  - 同様に `queueRegionPhys(...)` を追加。
  - `common_queue_used` と event buffer descriptor `addr` 計算を helper 経由に変更。
  - 非連続チェック停止コードを削除。

## 期待結果
- 起動ログで以下が出る:
  - `MouseDriver: queue ready`
  - `KeyboardDriver: queue ready`
- 以降、mouse/keyboard イベントログが継続して出力される。

## 検証
- `zig fmt kernel/user_programs/keyboard_driver.zig kernel/user_programs/mouse_driver.zig` 成功
- `zig build test` 成功
- `zig build efi` 成功
