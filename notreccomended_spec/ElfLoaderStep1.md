# ELF Loader Step1

## 目的
- 機械語直書きから脱却し、ユーザー空間プログラムを ELF でロードする基盤を作る。
- まずは `ELF64/x86_64/little-endian` の parser をカーネル側に導入する。

## 今回の実装
- `kernel/src/elf_loader.zig` を新規追加。
- 実装済み:
  - ELF ヘッダ検証 (`magic`, class, endian, machine, version, type)
  - Program Header 走査
  - `PT_LOAD` セグメント抽出
  - `filesz <= memsz` と範囲チェック
  - `Image` 構造へ entry/load segments を格納
- ブート時に `probe()` を実行し、parser が最低限動くことを確認。
  - ログ: `ELF loader step1 ready`

## 変更ファイル
- `kernel/src/elf_loader.zig` (新規)
- `kernel/src/main.zig`
  - `elf_loader` import
  - boot 初期化で `probe()` 実行

## 現在の状態
- FramebufferServer Step1 はデフォルト無効 (`enable_framebuffer_server_step1 = false`)。
- 現在の ring3 起動は idle task で安定優先。

## 次ステップ
- ELF バイナリをメモリに配置して `parse()` を実データに適用
- `PT_LOAD` を free list/CNode/PTE 経由で user 空間へ配置
- `entry` を ThreadContext に設定して `iretq` 起動

# ELF Loader Step2 (Embedded ELF Load)

## 目的
- `probe` だけでなく、実際に ELF イメージを user page へ展開して実行する。
- 機械語直書きの常用をやめ、`ELF -> PT_LOAD -> entry` の流れを確立する。

## 実装内容
- `kernel/src/elf_loader.zig`
  - `embeddedIdleElf()` を追加（最小 ELF64/ET_EXEC, 1x PT_LOAD）。
  - `loadToSinglePage(image, page_base_va, dest_page)` を追加。
    - ELF parse
    - PT_LOAD を page に copy
    - `filesz..memsz` はゼロクリア済み領域を利用
    - `entry` が PT_LOAD 範囲内であることを検証
- `kernel/src/main.zig`
  - ブート時ログを `ELF loader step2 ready (embedded image)` に更新。
  - `loadEmbeddedElfIntoUserPage()` を追加。
  - Framebuffer Step1 無効時:
    - `user_page` に embedded ELF を展開
    - `Thread0.frame.rip = e_entry` を設定
    - `enter ring3 with iretq (embedded ELF)` で起動

## 期待ログ
- `ELF loader step2 ready (embedded image)`
- `ELF image loaded`
- `entry=0x20000000`
- `load_segments=1`
- `enter ring3 with iretq (embedded ELF)`

## 現状の制約
- ローダー対象は「単一 page に収まる ELF」。
- `PT_LOAD` が page window を跨ぐケースは未対応。
- ファイルシステムからの ELF 読み込みは未実装（embedded 固定）。

## 次ステップ
- EFI ファイル読み込み (`EFI/BOOT/app.elf`) を追加。
- 複数ページ PT_LOAD のページ確保/マップを実装。
- Capability と統合し、`alloc -> CNode -> PTE` を ELF ロードにも適用。

# ELF Loader Step3 (Streaming Load)

## 目的
- ELF ローダーの入力を `[]const u8` 全展開前提から切り離し、`read_at(offset, out)` で段階的に読む方式へ移行する。
- 8KiB ロードウィンドウでも、`PT_LOAD` をチャンク転送してロード処理を維持する。

## 実装内容
- `kernel/src/elf_loader.zig`
  - `StreamReadError` を追加。
  - `Reader` を追加。
    - `context: *anyopaque`
    - `read_at: fn(context, offset, out) -> StreamReadError!void`
  - `StreamLoadToPageError` を追加 (`LoadToPageError || StreamReadError`)。
  - `parseFromReader()` を追加。
    - ELF ヘッダ + Program Header を `read_at` で逐次読み取り
    - `PT_LOAD` / `PT_DYNAMIC` の抽出
  - `copyFromReader()` を追加。
    - 512B チャンクで `PT_LOAD.p_filesz` を転送
  - `loadToSinglePageStreaming()` を追加。
    - parse -> PT_LOAD 転送 -> RELA(R_X86_64_RELATIVE) 適用 -> entry 検証
  - 既存 `loadToSinglePage()` は互換維持しつつ、内部でストリーミング経路を使うよう変更。
    - `[]const u8` から `Reader` へ接続する slice reader を追加

## 設計メモ
- `PT_LOAD` 転送は `filesz` 分のみコピーし、`memsz-filesz` は宛先ゼロ初期化で吸収する。
- `SourceOutOfRange` はセグメント範囲エラーにマップし、従来 API のエラー意味を保つ。
- `loadToSinglePageStreaming()` は入力ソース実装を固定しないため、将来のディスク直読みに流用できる。

## 現時点の制約
- 実行時マップ対象は従来どおり「単一/二重ページのロードウィンドウ内」。
- `main.zig` 側の UEFI ファイル読み込みは従来の事前ステージングのまま。
  - ただしローダー側はストリーミング API 化済みなので、次段でディスク直結しやすい状態。

## 次ステップ
- `main.zig` の `loadElfFromDisk()` を「全読み込み」から `Reader` 実装へ置換する。
- ExitBootServices 前後の設計を見直し、必要最小データだけを保持するロードフローにする。

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
