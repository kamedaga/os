# Capability Revoke Tree

## 目的
- capability の移譲先が多段化しても、root 側から回収（revoke）できるようにする。
- 追跡不能な権限リーク（DMAバッファ残存など）を防ぐ。

## 追加した中核
1. capability lineage
- `Capability` に以下を追加:
  - `cap_id`
  - `root_cap_id`
  - `parent_cap_id`
- `allocPageTo` は root capability を発行する（`parent_cap_id=0`）。
- `moveCap` は `cap_id/root/parent` を保持したまま所有者だけ移す。
- `grantCap` は child capability を新規発行する（`parent_cap_id=source.cap_id`）。

2. revoke tree
- `revokeCapTree(owner, paddr)` を追加。
- owner が持つ `paddr` の capability を起点に、`parent_cap_id` を辿って subtree 全体を削除する。
- 削除された各capについて `pte_sync_hook` を発火し、PTE整合性も維持する。

3. endpoint capability 連携
- `send_cap` は endpoint cap により宛先解決し、内部的には `moveCap` を通る。
- したがって send で移動した cap も lineage の追跡対象になる。

## syscall
- `sys_send_cap = 0x6` (`RSI=endpoint_id`)
- `sys_revoke_tree = 0x7` (`RDI=paddr`)
- `sys_grant_cap = 0x8` (`RDI=paddr`, `RSI=dst process id`, `RDX=rights bits`)

## 戻り値（主なもの）
- `RAX=0`: 成功
- `RAX=10`: revoke 失敗
- `RAX=11`: grant 失敗

## 期待される挙動
- 例:
  - `Process0` が root cap を保持
  - `grant` で `Process1` に child
  - `Process1` がさらに `grant`（将来 `Process2` へ）
- `revokeTree(Process0, capX)` で subtree が一括で無効化される。

## テスト
- `grantCap creates child and revokeCapTree at root removes descendants`
- `revokeCapTree from child only removes child subtree`
