# Process Creation Refactor (createUserProcess)

## 目的
- `main.zig` に散在していたプロセス生成の重複を共通化する。
- 今後のプロセス追加時に、`alloc -> address space -> thread init` の手順漏れを防ぐ。

## 追加した共通関数
- `createUserProcess(state, principal, thread_index, role_label) -> CreatedUserProcess`
  - `allocPageTo` で user page / user stack page を確保
  - `buildUserAddressSpaceFromCapabilities` を実行
  - `initThreadContext` を実行
  - 失敗時は既存と同じくログ出力して `hlt`

## 変更ファイル
- `kernel/src/main.zig`
  - `CreatedUserProcess` 構造体を追加
  - `createUserProcess` 関数を追加
  - 以下の起動経路で重複実装を `createUserProcess` 呼び出しに置換
    - MouseDriver (Process0)
    - BootLogConsole/Compositor owner (Process1)
    - BootLogSender (Process2)
    - DrawClient (Process0)
    - Framebuffer server (Process1)
    - 単独 user プロセス (Process0)

## 影響範囲
- 共通化対象は「プロセスの最小生成シーケンス」のみ。
- 追加ページ確保（mouse config/shared/vfb/boot log page など）や Capability 付与ロジックは従来どおり各分岐に残す。

## 次ステップ
1. `activateThread` まで含める共通化 (`createAndActivateUserProcess`) を検討。
2. 役割別セットアップ（mouse/compositor/bootlog）を関数分割し、`main()` の分岐を薄くする。
