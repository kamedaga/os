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
