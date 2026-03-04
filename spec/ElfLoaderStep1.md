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
