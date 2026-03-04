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
