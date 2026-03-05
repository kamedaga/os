# ELF Loader Step4 (Variable User Pages)

## 目的
- `loadUserElfIntoTwoPages` 固定をやめ、ELF の `PT_LOAD` サイズに応じて必要ページ数を可変で確保・配置する。

## 変更内容
- `kernel/src/main.zig`
  - 追加:
    - `computeUserElfRequiredBytes(image_bytes)`  
      ELF を parse して `max(vaddr + mem_size)` を算出し、4KiB アライン。
    - `loadUserElfIntoProcessPages(state, principal, page0_paddr, page1_paddr, image_bytes)`  
      必要ページ数に応じて:
      - page0/page1 へコピー
      - page2 以降は `allocPageTo` + `mapUserLinearRegion` で追加確保/マップしてコピー
    - `loadUserElfIntoProcessPagesOrHalt(...)`
  - 置換:
    - 旧 `loadUserElfIntoTwoPages*` 呼び出しを全て `loadUserElfIntoProcessPages*` へ変更
    - deferred compositor launch 経路も同様に置換
  - `user_elf_load_window` を `user_elf_max_size` サイズへ拡張。

## 現在の上限
- 現レイアウトでは `user_va + 0x3000` 以降を config/shared 用に予約しているため、
  - `user_program_max_load_bytes = 0x3000`（12KiB）を上限にしている。
- 12KiB超のELFを受けるには、次段で user VA レイアウトを再配置する必要がある。

## 検証
- `zig fmt src/main.zig` 成功
- `zig build test` 成功
- `zig build efi` 成功
