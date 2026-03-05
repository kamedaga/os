# Virtio Input Device Identification Split

## 目的
- virtio-input の候補から mouse と keyboard を分離して判定する。
- 「最初に見つかった入力デバイス」を流用する方式をやめ、デバイス能力で識別する。

## 実装
- `kernel/src/virtio_probe.zig`
  - `probeInputModern(..., InputKind)` を追加
  - `probeMouseModern` / `probeKeyboardModern` を分離
  - `device_cfg` の `EV_BITS` を参照して分類
    - mouse: `REL_X/REL_Y` または `ABS_X/ABS_Y + BTN_LEFT`
    - keyboard: `KEY_A` があり、`REL/ABS` の軸ビットを持たない
  - 判定ログ追加:
    - `virtio-probe: classify rel_xy=... abs_xy=... key_a=... btn_left=...`
- `kernel/src/main.zig`
  - keyboard 側を `probeKeyboardModern(...)` 呼び出しに変更
  - `mouse_modern_info` / `keyboard_modern_info` 型を `InputModernInfo` に統一

## 期待効果
- mouse 用プロセスと keyboard 用プロセスで、同じ候補デバイスを誤って掴むリスクを低減。
- BootLogConsole 経路で keyboard driver を起動する際に、mouse デバイス誤認が減る。

## 検証
- `zig build test` 成功
- `zig build efi` 成功
