---
tags:
  - pachaos
  - migration
  - capability
  - fd
  - phase0
---

# Capability to FD Migration Map

## 目的

現在の capability 系 API / 構造体を fd-based object model に移す対応表を作る。

Phase 0 ではこの表を使って、削除対象、互換 shim、先に fd 化する対象を決める。

## 大分類

| 現在 | 移行先 |
|---|---|
| page capability | kernel internal page allocation + VMO/VMA |
| VM object capability | `Vmo fd` |
| IPC buffer capability | `Vmo fd` + `Channel fd` |
| endpoint capability | `Endpoint fd` / `Channel fd` |
| capsule token | `Device` / `MmioRegion` / `DmaBuffer` / `Irq` fd |
| queue / command / iommu cap | capsule-derived fd |
| process builder token | `Process fd` / `AddressSpace fd` |
| ABI trap reply token | `Reply fd` / `Thread fd` / service protocol |
| PachaOS 専用 userland driver | kobox daemon or C service |
| userland Zig ABI helper | C header / C library |

## Memory syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_alloc_page` | 廃止 | page は ABI に出さない |
| `syscall_map_page` | `mmap(vmo_fd)` | paddr map ではなく VMO map |
| `syscall_map_page_anywhere` | `mmap(NULL, ...)` | libc-friendly にする |
| `syscall_alloc_map_pages` | `mmap(MAP_ANON)` | anonymous VMO |
| `syscall_alloc_map_pages_anywhere` | `mmap(NULL, MAP_ANON)` | anonymous VMA |
| `syscall_map_pages_batch` | `mmap` or VMO range map | batch page map は廃止 |
| `syscall_map_mmio` | `mmap(mmio_fd)` | MMIO fd 経由 |
| `syscall_install_mmio_cap` | `device_derive_mmio` | direct install を廃止 |
| `syscall_get_memory_stats` | `kernel_info` / debug service | ABI 本体から分離検討 |

## Page cap transfer syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_move_cap` | `fd_replace` / fd MOVE passing | page cap 固有操作は廃止 |
| `syscall_send_cap` | `ipc_send` with fd passing | fd passing に統合 |
| `syscall_grant_cap` | `fd_dup` / fd passing with rights | rights attenuation |
| `syscall_grant_caps_batch` | IPC fd array | batch は IPC message に統合 |
| `syscall_share_cap` | fd passing COPY | COPY transfer |
| `syscall_revoke_tree` | selective revoke only where needed | 全 page cap tree は廃止 |
| `syscall_drop_present` | `fd_close` / `munmap` | 操作対象で分離 |
| `syscall_accept_cap_transfer` | `ipc_recv` received fd array | transfer protocol 統合 |

## Endpoint / IPC syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_install_endpoint` | `ipc_endpoint_create` | fd object 化 |
| `syscall_signal_endpoint` | `ipc_send` / `event_signal` | endpoint signal を整理 |
| `syscall_recv_cap` | `ipc_recv` | fd receive |
| `syscall_grant_cap_on_endpoint` | `ipc_send` with fd | endpoint-specific grant を廃止 |
| `syscall_grant_caps_batch_on_endpoint` | `ipc_send` with fd array | 同上 |
| `syscall_ipc_call_reply_recv` | `ipc_call` | libipc 経由 |
| `syscall_ipc_call_reply_recv_fast` | libipc fast backend | syscall 直叩きしない |
| `syscall_wait_event` | `fd_wait` / `fd_poll` | wait primitive 統合 |

## VMO syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_create_vm_object_from_current_pages` | `vmo_create` / `vmo_clone` | current pages 依存を減らす |
| `syscall_map_vm_object` | `mmap(vmo_fd)` | VMO fd map |
| `syscall_grant_vm_object` | fd passing | VM object cap grant は廃止 |
| `syscall_release_vm_object` | `fd_close` + `munmap` | lifetime と mapping を分離 |
| `syscall_drop_vm_object` | `fd_close` | close に統合 |

Phase 3 では旧 VM object syscall は互換実装を持たず invalid を返す。`vmo_from_current_pages` は旧 token 生成ではなく VMO fd 生成であり、fd passing がない経路は Phase 4 まで未完成として扱う。

## Process builder syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_create_suspended_process` | `process_create` -> `Process fd` | token ではなく fd |
| `syscall_map_vm_object_to_process` | `mmap_into(process_fd, vmo_fd)` | `MAP_INTO` right |
| `syscall_alloc_map_pages_to_process` | `mmap_into(process_fd, MAP_ANON)` | page cap を経由しない |
| `syscall_set_process_initial_context` | `thread_set_context` | Thread/Process fd |
| `syscall_start_process` | `process_start` | Process fd |
| `syscall_abort_process` | `process_kill` / `process_abort` | Process fd |
| `syscall_copy_to_process` | debug/process write API | 制限付き |
| `syscall_copy_from_process_to_process` | debug/process copy API | 制限付き |
| `syscall_share_process_pages_to_process` | VMO fd passing / mmap_into | page share は廃止 |
| `syscall_mprotect_self` | `mprotect` | self operation |
| `syscall_map_vm_object_range_to_process` | `mmap_into` range | VMO fd |
| `syscall_set_process_bootstrap_owner` | process spawn protocol | 再設計 |
| `syscall_set_process_abi_trap_delegate` | service / trap protocol | 再設計 |

## Device / queue syscalls

| 旧 syscall | 新 API | 方針 |
|---|---|---|
| `syscall_register_iommu_driver` | device service / kernel boot policy | Phase 0 で再検討 |
| `syscall_iommu_authorize` | `DmaMapping fd` operation | IOMMU cap 廃止 |
| `syscall_dma_map_create` | `device_derive_dma_mapping` | fd object |
| `syscall_dma_map_set_state` | `dma_mapping_set_state` | fd object |
| `syscall_dma_map_release` | `fd_close(mapping_fd)` | close |
| `syscall_queue_submit` | device protocol / fast IPC | queue cap 固有 syscall を減らす |
| `syscall_queue_notify` | event / doorbell fd | notify fd |
| `syscall_command_authorize` | command rights on device/session fd | command cap 統合 |
| `syscall_derive_command_cap` | fd dup with narrowed rights | command cap tree 廃止 |
| `syscall_revoke_device_cap` | selective revoke | 必要 object のみ |

## Userland driver / kobox

既存の PachaOS 専用 driver は、kernel に寄せず、userland daemon として整理する。

置換方針。

| 現在 | 移行先 |
|---|---|
| PachaOS 専用 block driver | pachaos_capsule + kobox module daemon |
| capsule token env passing | device fd bootstrap / fd passing |
| driver direct syscall | `libcapsule` |
| driver-private IPC | `libipc` |
| Linux `.ko` support path | kobox daemon |

kobox は README に記載されている Linux Kernel Driver Runtime であり、PachaOS では driver daemon 基盤として扱う。

Phase 0 では、現在の kobox PachaOS backend が必要とする MMIO / DMA / IRQ / PCI config 操作を洗い出し、`libcapsule` の first API に反映する。

## Userland Zig 資産

userland は C / C ABI を主軸にする。

| 現在 | 移行先 |
|---|---|
| `userland/programs/*.zig` app | C service / C app |
| Zig ABI helper | C header / generated C header |
| Zig-only protocol helper | C 実装の `libipc` / service-specific C client |
| Zig rootfs / block helper | C daemon or kobox daemon |

一括削除はしない。fd-based ABI と C library が揃った領域から順に置き換える。

## Capsule syscalls

詳細は [[capsule-fd-spec]] を参照。

基本方針。

- token を fd に置換
- grant は fd passing
- close は `fd_close`
- MMIO / DMA / IRQ は派生 fd
- `libcapsule` を標準入口にする

## ABI trap syscalls

ABI trap は fd-based IPC と process/thread object に寄せる。

| 現在の系統 | 移行先 |
|---|---|
| reply target pages | VMO fd / mmap |
| reply token | Reply fd |
| copy_to/from trap target | process/thread debug or IPC payload |
| map reply target VM object | mmap into target |
| interrupt trap target | thread/process signal |

Linux ABI server 継続期間中は compatibility layer として残す。

native musl backend が動き始めた後、trap delegation の範囲を縮小する。

## 削除順序

1. memory fast path から page cap を外す
2. VMO cap を VMO fd に置換する
3. IPC transfer を fd passing にする
4. IPC buffer cap を VMO + Channel に統合する
5. capsule token を fd にする
6. queue / command / iommu cap を capsule-derived fd にする
7. process builder token を Process fd にする
8. old capability syscall を compatibility file に隔離する
9. tests を fd-based に移す
10. old capability tables を削除する

## Phase 0 決定事項

- どの old syscall を最初に compatibility shim 化するか
- syscall number を新規 range にするか既存 range を再利用するか
- Linux ABI server をいつまで primary target にするか
- native musl smoke の最初の target
- old capability tests の置換戦略
- kobox daemon 化する driver の最初の target
- userland Zig 資産の置換順序
