---
tags:
  - pachaos
  - phase0
  - roadmap
---

# Phase 0 Checklist

## 目的

fd-based object microkernel への移行を始める前に、設計上の未確定点を潰す。

Phase 0 では kernel 実装を大きく変更しない。

成果物は仕様 md、対応表、最初の実装順序である。

## 完了条件

- fd の意味が決まっている
- object type の初期集合が決まっている
- rights bit の分類が決まっている
- fd passing IPC の最小意味論が決まっている
- memory ABI の最小 syscall が決まっている
- capsule fd API の最小形が決まっている
- userland C ABI / header 方針が決まっている
- kobox backend を `libcapsule` へ載せるための最小 API が決まっている
- old capability API から fd API への移行表がある
- Phase 1 の最初の PR / commit scope が決まっている

Phase 1 に進むための固定決定は [[phase0-design-freeze]] にまとめる。

## 読む設計資料

- [[fd-based-refactor-roadmap]]
- [[phase0-design-freeze]]
- [[phase1-fd-table-core-plan]]
- [[fd-abi-spec]]
- [[ipc-abi-spec]]
- [[memory-abi-spec]]
- [[capsule-fd-spec]]
- [[capability-to-fd-migration-map]]

## 決めること

### fd core

- fd number type
- fd table max / growth policy
- rights bit width
- fd flags
- object id / generation format
- refcount ownership
- close-on-exec semantics
- spawn inheritance semantics

### object model

- initial `ObjectKind`
- object allocation storage
- generation wrap handling
- debug inspect format
- object-specific operation dispatch
- selective revoke が必要な object

### IPC

- Endpoint / Channel / Reply の分離
- max inline payload
- max fd transfer count
- blocking / timeout unit
- fd transfer `COPY` / `MOVE`
- rights attenuation encoding
- readiness bits
- libipc public API
- fast IPC feature negotiation

### Memory

- VMO create flags
- anonymous mmap policy
- page fault zero-fill policy
- VMA count limit
- mmap address selection を kernel がどこまで行うか
- `MAP_PRIVATE` / COW の初期扱い
- pkey を VMA に入れるか
- file-backed VMO と pager の境界

### Capsule

- Device fd discovery
- MMIO を generic mmap に統合するか
- DMA buffer を VMO subtype にするか
- IRQ wait / ack semantics
- IOMMU mapping state
- libcapsule first API
- kobox backend の必要最小 API
- kobox daemon に device fd を渡す bootstrap protocol
- README と現在の kobox 起動経路から必要 operation を抽出する

### Userland C Policy

- native ABI header の置き場所
- `libpacha`, `libipc`, `libcapsule` の build / install path
- `libpacha`, `libipc`, `libcapsule` を C で実装する方針
- musl backend から呼ぶ public C ABI
- existing userland Zig app の置換順序
- Zig ABI helper を C header / generated header に移す方針
- C service と kobox daemon を標準 userland component とする方針

### Migration

- 新 syscall range
- old syscall compatibility policy
- tests の置換順序
- Linux ABI server と native musl backend の並行期間
- README の説明変更タイミング
- kobox daemon に置換する既存 driver の順序
- userland Zig build path の削除タイミング

## Phase 1 候補

最初の実装 scope は小さくする。

候補 A: fd table core のみ。

- `FdEntry`
- object id / generation
- close / dup
- debug inspect
- tests

候補 B: VMO fd 最小実装。

- anonymous VMO fd
- mmap anonymous
- munmap
- mprotect
- page cap bypass path
- tests

候補 C: IPC fd passing 最小実装。

- endpoint fd
- send / recv
- fd transfer
- rights attenuation
- libipc stub
- tests

推奨は A -> B -> C。

理由は、fd table core がないと VMO fd も IPC fd passing も一時実装になりやすいため。

## Phase 0 でやらないこと

- kernel memory manager の大規模変更
- old capability syscall の削除
- capsule 実装の置換
- pkey fast IPC 実装
- musl backend 実装
- Linux ABI server の削除
- userland Zig 資産の一括削除
- 既存 driver の一括 kobox 化

## 重要な設計原則

kernel に入れる前に確認する。

- それは userland library でできないか
- file semantics が kernel に入っていないか
- authority は fd rights で表現できているか
- paddr / page を userland ABI に出していないか
- syscall 直叩きではなく library 経由にできるか
- fd object として lifetime が説明できるか
