# IPC Capability Step1: `send_cap` (Move-Only)

## 目的
- IPC の最小機能として、capability を **copy せず move のみ** で渡す。
- 「Capability は Process に属する」という既存原則を維持する。

## 追加したもの
- kernel 内部 API:
  - `KernelState.sendCap(from, to, paddr)`
- syscall:
  - `sys_send_cap = 0x6`
  - `RDI = paddr`
  - `RSI = dst process id` (`0=Process0`, `1=Process1`)

## `send_cap` の仕様
- `from` と `to` は Process のみ許可（`Device0` は不可）。
- `from == to` は拒否。
- `from` が `paddr` capability を持たない場合は拒否。
- rights は送信元 capability の値をそのまま保持して移譲する。
- 結果として capability は送信元から消え、送信先にのみ残る（move-only）。

## syscall 戻り値
- `RAX=0`: 成功
- `RAX=8`: `send_cap` 失敗（invalid state / capability not found など）
- `RAX=1`: 不正引数（dst process id が 0/1 以外）

## 整合性
- `send_cap` は内部で `moveCap` を利用するため、既存の `pte_sync_hook` が必ず発火する。
- これにより capability 移譲時の PTE 同期ルール（Strict PTE-Capability Synchronization）をそのまま満たす。

## テスト追加
- `sendCap moves capability process to process with rights preserved`
  - Process0 -> Process1 へ移譲し、単一所有と rights 保持を確認。
- `sendCap rejects non-process endpoints`
  - Process/Device 間の送信を拒否することを確認。

## 次ステップ候補
- 受信キュー（mailbox）導入:
  - `send_cap` は「即時移譲」ではなく、宛先 process の受信キューへ enqueue。
- `recv_cap` 追加:
  - 受信側が明示的に dequeue して CNode に取り込む。
- thread 単位待機:
  - `recv_cap` ブロックと scheduler 連携。
