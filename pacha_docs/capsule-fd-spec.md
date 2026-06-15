---
tags:
  - pachaos
  - abi
  - capsule
  - fd
  - phase0
---

# Capsule FD Spec Draft

## 目的

capsule を fd-based object model に統合する。

`capsule token` を直接 userland が扱う設計から、device authority を fd として扱う設計へ移行する。

```text
Device fd = hardware authority
```

driver と kobox backend は syscall を直接叩かず、`libcapsule` を使う。

## Object

想定 object。

```text
Device fd
MmioRegion fd
DmaBuffer fd
DmaMapping fd
Irq fd
EventQueue fd
```

旧 capsule kind との対応。

| 旧 CapsuleKind | 新 fd object |
|---|---|
| `session` | `Device` or service `Channel` |
| `device` | `Device fd` |
| `mmio` | `MmioRegion fd` |
| `dma_buffer` | `DmaBuffer fd` |
| `dma_mapping` | `DmaMapping fd` |
| `irq` | `Irq fd` |
| `event_queue` | `EventQueue fd` or `Event fd` |

## Rights

Device fd rights。

| Right | 意味 |
|---|---|
| `QUERY` | device snapshot / info |
| `CONFIG_READ` | PCI config read |
| `CONFIG_WRITE` | PCI config write |
| `BAR_INFO` | BAR 情報取得 |
| `DERIVE_MMIO` | MMIO region fd 派生 |
| `DERIVE_DMA_BUFFER` | DMA buffer fd 派生 |
| `DERIVE_DMA_MAPPING` | DMA mapping fd 派生 |
| `DERIVE_IRQ` | IRQ fd 派生 |
| `BUS_MASTER` | bus mastering |
| `RESET` | device reset |
| `POWER` | power management |
| `HOTPLUG_OBSERVE` | hotplug observe |

MmioRegion fd rights。

| Right | 意味 |
|---|---|
| `MMIO_MAP_READ` | readable mapping |
| `MMIO_MAP_WRITE` | writable mapping |

DmaBuffer fd rights。

| Right | 意味 |
|---|---|
| `CPU_READ` | CPU read |
| `CPU_WRITE` | CPU write |
| `DMA_READ` | device reads memory |
| `DMA_WRITE` | device writes memory |
| `MAP` | user mapping |
| `SHARE` | fd passing |

DmaMapping fd rights。

| Right | 意味 |
|---|---|
| `ENABLE` | IOMMU mapping enable |
| `DISABLE` | IOMMU mapping disable |
| `SYNC` | state sync |
| `RELEASE` | mapping release |

Irq fd rights。

| Right | 意味 |
|---|---|
| `IRQ_WAIT` | IRQ wait |
| `IRQ_ACK` | acknowledge |
| `IRQ_MASK` | mask / unmask |

## DMA の原則

CPU 権限と device 権限を分ける。

```text
CPU_WRITE != DMA_WRITE
MAP_WRITE != DMA_WRITE
```

device が memory を読むには `DMA_READ` が必要。

device が memory に書くには `DMA_WRITE` が必要。

userland が buffer に書くには `CPU_WRITE` が必要。

## Syscall 案

kernel syscall は fd object operation として提供する。

```text
device_query(device_fd, out_info)
device_pci_config_read(device_fd, offset, width) -> value
device_pci_config_write(device_fd, offset, width, value)
device_pci_bar_info(device_fd, bar, out_info)

device_derive_mmio(device_fd, bar, offset, size, rights) -> mmio_fd
device_derive_dma_buffer(device_fd, size, flags, rights) -> dma_buffer_fd
device_derive_dma_mapping(device_fd, dma_buffer_fd, iova, size, direction, flags) -> dma_mapping_fd
device_derive_irq(device_fd, index, flags) -> irq_fd

mmio_mmap(mmio_fd, addr, size, prot, flags) -> va
dma_buffer_mmap(dma_buffer_fd, addr, size, prot, flags) -> va
dma_mapping_set_state(mapping_fd, state)
irq_wait(irq_fd, timeout) -> count
irq_ack(irq_fd)
```

`mmio_mmap` と `dma_buffer_mmap` は汎用 `mmap(fd)` に統合してもよい。

Phase 0 で、device object 固有 syscall と汎用 mmap の境界を決める。

## libcapsule

driver / kobox backend は `libcapsule` を使う。

`libcapsule` は C で実装し、C ABI として提供する。PachaOS native userland、musl backend、kobox PachaOS backend から同じ header を使えるようにする。

kobox については README の kobox セクションを入口にし、現在の PachaOS Capsule backend が要求している device access をこの fd API に置き換える。

目標。

- kobox backend は `Device fd` から MMIO / DMA / IRQ / PCI config を派生する
- kobox backend は capsule token を直接扱わない
- kobox modules は driver daemon として起動し、必要な device fd を bootstrap / IPC で受け取る
- kobox daemon と FS / block / net / input service の連携は `libipc` で行う
- 既存の PachaOS 専用 driver は、kobox で置換できるものから段階的に退役させる

API 案。

```c
int capsule_query(int dev_fd, struct capsule_info *out);
int capsule_pci_config_read(int dev_fd, uint16_t off, unsigned width, uint32_t *out);
int capsule_pci_config_write(int dev_fd, uint16_t off, unsigned width, uint32_t value);
int capsule_pci_bar_info(int dev_fd, unsigned bar, struct capsule_bar_info *out);

int capsule_derive_mmio(int dev_fd, unsigned bar, uint64_t off, size_t len, uint64_t rights);
int capsule_derive_dma_buffer(int dev_fd, size_t size, uint64_t flags, uint64_t rights);
int capsule_derive_dma_mapping(int dev_fd, int dma_fd, uint64_t iova, size_t len, unsigned direction);
int capsule_derive_irq(int dev_fd, unsigned index, uint64_t flags);

int capsule_map_mmio(int mmio_fd, void **addr, size_t *len, int prot);
int capsule_map_dma(int dma_fd, void **addr, size_t *len, int prot);
int capsule_irq_wait(int irq_fd, uint64_t timeout_ns);
int capsule_irq_ack(int irq_fd);
```

## 旧 syscall との対応

| 旧 syscall | 新 API |
|---|---|
| `syscall_capsule_query` | `device_query` / `capsule_query` |
| `syscall_capsule_derive_mmio` | `device_derive_mmio` |
| `syscall_capsule_derive_dma_buffer` | `device_derive_dma_buffer` |
| `syscall_capsule_derive_dma_mapping` | `device_derive_dma_mapping` |
| `syscall_capsule_derive_dma_mapping_from_buffer` | `device_derive_dma_mapping` |
| `syscall_capsule_derive_irq` | `device_derive_irq` |
| `syscall_capsule_grant` | fd passing with rights attenuation |
| `syscall_capsule_revoke` | fd close / selective revoke |
| `syscall_capsule_close` | `fd_close` |
| `syscall_capsule_pci_config_read` | `device_pci_config_read` |
| `syscall_capsule_pci_config_write` | `device_pci_config_write` |
| `syscall_capsule_pci_bar_info` | `device_pci_bar_info` |
| `syscall_capsule_irq_poll` | `irq_wait` / `fd_poll` |

## Phase 0 決定事項

- capsule token と fd の併存期間
- device discovery を service にするか kernel query にするか
- MMIO mapping を generic mmap に統合するか
- DMA buffer を VMO subtype にするか独立 object にするか
- IOMMU mapping state の最小 API
- IRQ の ack / mask semantics
- kobox backend の最小要求 API
- kobox daemon への device fd bootstrap 方法
- kobox backend が必要とする `libcapsule` header の安定範囲
