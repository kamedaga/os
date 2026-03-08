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

# IPC Capability Step2: Endpoint Capability

## 目的
- IPC の送信先指定を `process id` 直指定から廃止し、`endpoint capability` に昇格する。
- `send_cap` は「送信者が保持する endpoint cap が示す宛先」にのみ送れる。

## 変更点
1. endpoint cap テーブルを追加
- `KernelState.endpoint_tables` を追加。
- endpoint cap は `EndpointCapability { endpoint_id, target }`。
- 初期状態で以下を配布:
  - `Process0`: `ep=0x11 -> Process1`
  - `Process1`: `ep=0x10 -> Process0`

2. kernel API を追加
- `endpointTargetFor(owner, endpoint_id) -> ?PrincipalId`
- `sendCapOnEndpoint(from, endpoint_id, paddr)`
  - owner の endpoint cap を解決
  - 解決先 target へ `sendCap`（move-only）を実行

3. syscall ABI 変更
- `sys_send_cap = 0x6`
  - `RDI = paddr`
  - `RSI = endpoint_id`（以前の dst process id ではない）
- 戻り値:
  - `0`: 成功
  - `9`: endpoint cap 不在
  - `8`: その他送信失敗（capability not found / invalid state など）

## 不変条件
- endpoint cap を持たない process は、その endpoint 経由で送信できない。
- page capability は move-only（二重所有なし）。
- `sendCapOnEndpoint` は内部で `sendCap -> moveCap` を通るため、PTE同期フックも維持される。

## デモ（cap transfer）
- 専用デモで `p0->p1->p0` を endpoint 経由で実行。
- ログに endpoint 情報を表示:
  - `Process0 endpoints: ep=0x11 -> Process1`
  - `Process1 endpoints: ep=0x10 -> Process0`
- 送信ログ:
  - `send_cap from=Process0 to=Process1 ep=0x11 paddr=...`
  - `send_cap from=Process1 to=Process0 ep=0x10 paddr=...`

## テスト
- `sendCapOnEndpoint requires endpoint capability`
- `sendCapOnEndpoint rejects missing endpoint`
