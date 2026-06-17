---
tags:
  - pachaos
  - abi
  - capsule
  - fd
  - phase0
---

# Capsule FD Spec

## 目的

capsule を fd-based object model に統合する。

`capsule token` を直接 userland が扱う設計をやめ、device authority を fd として扱う。

Phase 6 では kernel 内部 authority も token table ではなく fd object payload と per-fd rights に寄せる。旧 capsule token は互換対象ではない。

```text
Device fd = hardware authority
```

driver と将来の kobox backend は syscall を直接叩かず、`libcapsule` を使う。

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

fd object の対応。

| fd object | 意味 |
|---|---|
| `Device fd` | PCI/device authority |
| `MmioRegion fd` | derived MMIO mapping authority |
| `DmaBuffer fd` | CPU-visible DMA buffer authority |
| `DmaMapping fd` | device-visible IOVA mapping authority |
| `Irq fd` | interrupt wait/ack authority |
| `EventQueue fd` | later event queue authority |

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

## Syscall

kernel syscall は fd object operation として提供する。番号は既存 `syscall_capsule_*` range を再利用するが、意味論は token ではなく fd である。

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

現在の Phase 6 実装では `device_derive_mmio(device_fd, bar, user_va, size, flags)` が mapping と `MmioRegion fd` 作成を同時に行う。後で `mmap(mmio_fd)` に分ける余地は残すが、userland から token は見せない。

## libcapsule

driver / kobox backend は `libcapsule` を使う。

`libcapsule` は C で実装し、C ABI として提供する。PachaOS native userland、musl backend、kobox PachaOS backend から同じ header を使えるようにする。

`libcapsule` 自体は inline syscall asm を持たず、低レベル syscall entry は `libpacha` に集約する。これにより capsule API は hardware authority の意味論に集中し、`syscall` / `sysret` の ABI 詳細を driver や kobox backend へ漏らさない。

kobox については README の kobox セクションを入口にし、現在の PachaOS Capsule backend が要求している device access をこの fd API に置き換える。

ただし kobox daemon 起動は init / supervision の修正を伴うため Phase 6 では行わない。musl native backend が入る Phase 7 以降に、device fd bootstrap と daemon IPC protocol を実装する。

目標。

- kobox backend は `Device fd` から MMIO / DMA / IRQ / PCI config を派生する
- kobox backend は capsule token を直接扱わない
- kobox modules は Phase 7 以降に driver daemon として起動し、必要な device fd を bootstrap / IPC で受け取る
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
| `syscall_capsule_query` | `device_query` / `capsule_query(fd)` |
| `syscall_capsule_derive_mmio` | `device_derive_mmio(device_fd)` -> `MmioRegion fd` |
| `syscall_capsule_derive_dma_buffer` | `device_derive_dma_buffer(device_fd)` -> `DmaBuffer fd` |
| `syscall_capsule_derive_dma_mapping` | `device_derive_dma_mapping(device_fd)` -> `DmaMapping fd` |
| `syscall_capsule_derive_dma_mapping_from_buffer` | `device_derive_dma_mapping(dma_buffer_fd)` -> `DmaMapping fd` |
| `syscall_capsule_derive_irq` | `device_derive_irq(device_fd)` -> `Irq fd` |
| `syscall_capsule_grant` | process fd bootstrap / fd transfer。token grant ではない |
| `syscall_capsule_revoke` | fd close / later selective revoke |
| `syscall_capsule_close` | fd close |
| `syscall_capsule_pci_config_read` | `device_pci_config_read(device_fd)` |
| `syscall_capsule_pci_config_write` | `device_pci_config_write(device_fd)` |
| `syscall_capsule_pci_bar_info` | `device_pci_bar_info(device_fd)` |
| `syscall_capsule_irq_poll` | `irq_wait(irq_fd)` / later `fd_poll` |

## Phase 6 決定事項

- capsule token と fd は併存させない
- `KernelState` runtime authority は fd object payload を正とする
- `Device` / `MmioRegion` / `DmaBuffer` / `DmaMapping` / `Irq` を `KernelObjectKind` にする
- cleanup は fd object release に寄せる
- `libcapsule` は C 実装として `userland/libcapsule` に置く
- raw syscall backend は `libpacha` に置き、`libcapsule` はそれを使う
- Phase 6 の smoke test は seed を通さず、bootfs の `device-fd-smoke` で初期 device fd と `libcapsule` query を確認する
- kobox daemon への device fd bootstrap は Phase 7 以降に送る
