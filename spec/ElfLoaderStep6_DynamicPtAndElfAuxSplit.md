# ELF Loader Step6: Dynamic PT + ELF/Aux Split

## Goal
- `ELF領域` と `aux領域(config/shared/fb)` を明確に分離する
- `ELF領域` を固定 `pt_slot_count` 依存から外し、必要な PD/PT だけを使う
- 大きい `.bss` を持つユーザELF（例: compositor）でも起動可能にする

## What Changed

### 1. UserAddressSpace の固定 PT スロットを廃止
- Before:
  - `pts: [pt_slot_count][512]u64`（固定）
  - `pt_slot_count = 4` 前提
- After:
  - `pt_pages: [max_dynamic_pt_pages][512]u64`
  - `pt_page_pd_index: [max_dynamic_pt_pages]u16`
  - `pd_index -> pt_page` の対応を動的に確保

対象:
- `kernel/src/capability.zig`
- `kernel/src/main.zig`

### 2. map 系のロジックを PD index ベースへ変更
- `va -> pd_index/pt_index` を都度算出
- 必要なときだけ `ensurePtSlotForPd(...)` で PT を作成
- alias 除去、drop present、rights sync も「使用中 PT のみ」走査

対象:
- `capability.lookupUserMappedPaddrForVa`
- `capability.mapUserPageFromCapability`
- `capability.dropPresentForUserMappedPaddr`
- `capability.syncPageTableRightsForPaddr`
- `main.mapUserLinearRegion`
- `main.buildUserAddressSpace`

### 3. ELF領域と aux領域の間隔を拡張
- `user_aux_base_va = user_va + 0x60_0000`
- 結果として `user_program_max_load_bytes` は 6 MiB

対象:
- `kernel/src/main.zig`

### 4. user program 側の固定 VA を追従
- `0x2060_1000` boot log
- `0x2060_2000` config
- `0x2060_3000` shared
- `0x2060_4000` virtual framebuffer
- `0x2060_5000` framebuffer

対象:
- `kernel/user_programs/*.zig`（該当定数）

## Why This Works
- 以前は `pt_slot_count` の上限を超える VA が map できず、ELF 読み込み上限が実質固定だった
- いまは PD ごとに PT を動的に割り当てるため、ELFサイズ増大に追従できる
- aux を高位へ退避したことで、ELF の連続ロード窓を十分確保できる

## Verified
- `zig build test` succeeded
- `zig build efi` succeeded

## Next
- `deferred compositor launch` の最終動作確認
- 必要なら `user_program_max_load_bytes` をさらに拡張（例: 8 MiB 以上）
