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
