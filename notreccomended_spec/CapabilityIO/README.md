# CapabilityIO

## 目的
- DMA に使う排他的 capability ページの状態遷移を明文化する。
- `kernel.zig` の `moveCap` / `startDma` / `completeDma` の意味論に矛盾がないかを TLA+ で検査する。
- 将来の `Capability + IOMMU` DMA マッピングモデルの土台を作る。

## スコープ
- 対象は「排他的に move される DMA 用ページ」だけ。
- `grantCap` による共有 lineage はこのモデルでは扱わない。
- 共有 revoke tree は [CapabilityRevokeTree.md](/c:/Users/kamer/Documents/CapabilityOS/spec/CapabilityRevokeTree.md) 側の責務とする。

## いまのコードと対応する箇所
- `moveCap`: [kernel.zig](/c:/Users/kamer/Documents/CapabilityOS/kernel/src/kernel.zig#L950)
- `startDma`: [kernel.zig](/c:/Users/kamer/Documents/CapabilityOS/kernel/src/kernel.zig#L721)
- `completeDma`: [kernel.zig](/c:/Users/kamer/Documents/CapabilityOS/kernel/src/kernel.zig#L740)
- rights subset 判定: [kernel.zig](/c:/Users/kamer/Documents/CapabilityOS/kernel/src/kernel.zig#L599)

## モデルの意図
- `Process0`, `Process1`, `Device0` の 3 主体を置く。
- 1 ページだけを追跡し、排他的 move のみを許す。
- `StartDma` は `Process0 -> Device0` へ `{"dma"}` を渡す。
- `CompleteDma` は `startDma` 時点で保存した rights を `Process0` に返す。
- `ExclusiveMove` は source rights の subset だけを許す。
- `Device0` へ move できる rights は `{"dma"}` のみとする。

## TLA+ で検査したい性質
- `TypeInv`
  状態変数の型が壊れない。
- `ExclusiveHolder`
  対象ページは常に 1 主体だけが持つ。
- `NoViolations`
  以下の violation が 1 つも起きない。
  - `MoveEscalatesRights`
  - `DeviceGetsCpuRights`
  - `CompleteDmaRestoresDifferentRights`

## 現在のモデルで表していること
1. `moveCap` は rights subset のときだけ成立する
- source cap より強い rights への move は不許可。

2. `moveCap` の `Device0` 宛ては DMA-only
- `cpu_read` / `cpu_write` 付きで `Device0` へ移せない。

3. `startDma` は元 rights を保存する
- `Process0` から `Device0` へ渡す前に restore 用 rights を保存する。

4. `completeDma` は保存された rights を復元する
- DMA 完了時は固定 rights ではなく、`startDma` 時点の rights を `Process0` に戻す。

## 期待する使い方
現状の `CapabilityIO.tla` は、いまの kernel 実装に合わせて

- `moveCap` の subset 制約
- `moveCap` の `Device0` 宛て DMA-only 制約
- `startDma` の rights 保存
- `completeDma` の rights 復元

までを含んでいる。

期待値:
1. `TypeInv` が通る。
2. `ExclusiveHolder` が通る。
3. `NoViolations` が通る。

## TLC 実行
WSL 側に `tlc` を入れているので、`spec/CapabilityIO` では次でモデルチェックできる。

```bash
cd /mnt/c/Users/kamer/Documents/CapabilityOS/spec/CapabilityIO
~/.local/bin/tlc -config CapabilityIO.cfg CapabilityIO.tla
```

## 次の設計拡張
- `DmaMapToken` を状態に追加して、`どの device domain にどの purpose で map したか` を表現する。
- `Device0` を `GpuControl`, `GpuCursor`, `Net0` のような device domain に分割する。
- page 単位ではなく scatter/gather list 単位へ拡張する。
