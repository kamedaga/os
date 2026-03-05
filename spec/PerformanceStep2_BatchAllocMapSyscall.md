# Performance Step2: Batch alloc/map syscall

## Goal
- 起動初期の `int 0x80` 回数を減らし、`queue ready` までの遅延を短縮する

## Added syscall
- `syscall_alloc_map_pages = 0xC`
- 引数:
  - `rdi`: `base_va` (ページ境界)
  - `rsi`: `page_count` (`1..64`)
  - `rdx`: `writable` (`bit0`)
  - `rcx`: `out_paddr_list_va` (`u64[page_count]` のユーザVA、不要なら `0`)
- 戻り値:
  - `0`: success
  - それ以外: 既存 `syscall_err_*`

## Kernel changes
- `kernel/src/main.zig`
  - `syscall_alloc_map_pages` を追加
  - 1回の syscall で:
    - `allocPageTo`
    - `mapUserPageFromCapability`
    - 必要なら `paddr` 配列をユーザメモリへ書き戻し
  - ユーザメモリ書き戻し用に `copyBytesToUserVa` / `writeUserU64` を追加

## User program changes
- `kernel/user_programs/mouse_driver.zig`
- `kernel/user_programs/keyboard_driver.zig`
  - queue 用 2ページ確保を
    - before: `allocPage x2 + mapPage x2`
    - after: `allocMapPages(..., page_count=2, out_paddr_list_va=...)`

## Expected effect
- 各ドライバの queue 準備で syscall 回数を `4 -> 1` に削減
- `Thread3: KeyboardDriver: queue ready` までの待ち時間短縮

## Verification
- `zig build efi` succeeded
- `zig build test` succeeded

## Log ordering tweak
- `send_cap from=Process0 ...` の直前に framebuffer サマリを1回出力
  - `framebuffer before send_cap`
  - `fb_paddr`, `fb_size`, `fb_resolution`, `fb_pitch`
- `enter ring3 with iretq ...` を BootLog 描画に反映するため、
  ring3 直前に `publishBootLogToUserPage(...)` を再実行
- BootLog page へは「先頭」ではなく「最新末尾」を詰めるように変更
  - 4KiB制限内で最新ログ（`enter ring3...` など）が見えるようにした

## Send-cap ordering fix (Mouse first)
- `kernel/user_programs/mouse_driver.zig`
  - `shared_page_paddr` 検証と `sendCap(shared_page_paddr, endpoint_to_process1)` を
    queue 初期化より前へ移動
  - `queue ready` 後の `switchThread(3)` を削除
- 目的:
  - `send_cap from=Process0 ...` を `KeyboardDriver: queue ready` より前に出しやすくし、
    compositor への shared-page 受け渡し開始を前倒しする

## BootLog ordering consistency fix
- `kernel/src/main.zig`
  - `serialWriteFmt(...)` を追加し、1行/1ブロックを単発出力に統一
  - `syscall_send_cap` のログを分割 `serialWrite + printHex` から単発フォーマット出力へ変更
  - `framebuffer cap ...` と `framebuffer before send_cap` も単発フォーマット出力へ変更
- 目的:
  - ターミナル順序と BootLog 画面順序のズレ（分割出力中の見かけ上の並び替え）を抑える
