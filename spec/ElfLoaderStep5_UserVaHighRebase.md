# ELF Loader Step5 (High VA Rebase)

## 目的
- `config/shared/fb` のユーザVAを高位へ再配置し、ELFロード可能領域を拡張する。
- `PT` 1枚前提を拡張し、`user_va` から連続2スロット（合計4MiB）を扱えるようにする。

## 変更内容
- `kernel/src/capability.zig`
  - `UserAddressSpace` を `pt` 単体から `pts[2][512]` へ拡張。
  - `userPtSlotAndIndexForVa()` を追加し、VA→PTスロット解決を統一。
  - 以下APIを2スロット対応:
    - `lookupUserMappedPaddrForVa`
    - `mapUserPageFromCapability`
    - `dropPresentForUserMappedPaddr`
    - `syncPageTableRightsForPaddr`
- `kernel/src/main.zig`
  - VA再配置:
    - `user_aux_base_va = user_va + 0x20_0000`
    - `boot_log_user_va = user_aux_base_va + 0x1000`
    - `mouse_driver_config_va = user_aux_base_va + 0x2000`
    - `mouse_shared_driver_va = user_aux_base_va + 0x3000`
    - `virtual_framebuffer_*_va = user_aux_base_va + 0x4000`
    - `framebuffer_user_va = user_aux_base_va + 0x5000`
  - `user_program_max_load_bytes` を `user_aux_base_va - user_va`（2MiB）へ拡張。
  - `buildUserAddressSpace` を複数PTスロット初期化・PD接続対応に変更。
  - `mapUserLinearRegion` を複数PD（PTスロット）へ跨るマップ対応に変更。
- `kernel/user_programs/*`
  - `boot_log_console`, `mouse_driver`, `keyboard_driver`, `compositor`,
    `bootlog_sender`, `framebuffer_server`, `mouse_draw` の固定VA定数を再配置後VAへ更新。

## 効果
- ELFロード上限が 12KiB (`0x3000`) から 2MiB (`0x20_0000`) に拡張。
- framebuffer/config/shared を高位スロットへ逃がしたため、低位スロットをELF用に確保できる。

## 検証
- `zig fmt` 成功
- `zig build test` 成功
- `zig build efi` 成功
