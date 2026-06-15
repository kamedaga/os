---
tags:
  - pachaos
  - abi
  - memory
  - vmo
  - phase0
---

# Memory ABI Spec Draft

## 目的

page capability を userland ABI の主役から外し、VMO / VMA / mmap に整理する。

page は kernel 内部の実装単位であり、userland に見せる authority 単位ではない。

## 境界

kernel が管理するもの。

- physical page allocator
- address space
- page table
- VMO object
- VMA mapping ledger
- mmap / munmap / mprotect
- page fault の最低限処理
- fd rights による map 権限判定

libc / userland が管理するもの。

- malloc
- brk 互換
- mmap address hint policy
- file-backed mmap の意味
- ELF loader layout
- stack / heap policy
- shared library layout
- page cache policy

## VMO

VMO は memory backing store を表す kernel object。

```text
Vmo fd
  size
  flags
  backing kind
  rights
```

backing kind。

```text
anonymous
zero-fill-on-demand
physical-contiguous
device-dma
pager-backed
```

初期実装では `anonymous` と `zero-fill-on-demand` を優先する。

## VMA

VMA は process address space 内の mapping record。

```c
struct vma {
    uintptr_t start;
    size_t len;
    int prot;
    int flags;
    int vmo_fd;
    uint64_t offset;
    int pkey;
};
```

kernel の VMA は POSIX mmap の全意味論ではなく、mapping ledger である。

## Rights

VMO fd rights。

| Right | 意味 |
|---|---|
| `MAP_READ` | readable mapping を作れる |
| `MAP_WRITE` | writable mapping を作れる |
| `MAP_EXEC` | executable mapping を作れる |
| `RESIZE` | VMO size を変更できる |
| `SHARE` | shared fd passing / shared mapping を許す |
| `PAGER_ATTACH` | pager を接続する |

VMA protection は VMO fd rights の subset でなければならない。

```text
VMA prot <= VMO fd rights
```

## Syscall 案

```text
vmo_create(size, flags) -> vmo_fd
vmo_from_current_pages(addr, size, rights, flags) -> vmo_fd
vmo_resize(vmo_fd, new_size)
vmo_get_info(vmo_fd, out_info)
vmo_clone(vmo_fd, offset, size, rights, flags) -> new_vmo_fd

mmap(addr, size, prot, flags, vmo_fd, offset) -> mapped_addr
munmap(addr, size)
mprotect(addr, size, prot)
msync(addr, size, flags)
```

`vmo_clone` は rights attenuation と subrange view のために使う。

Phase 3 の実装済み subset は `vmo_create`, `vmo_from_current_pages`, `mmap`, `munmap` である。

## mmap flags

初期集合。

```text
MAP_FIXED
MAP_FIXED_NOREPLACE
MAP_PRIVATE
MAP_SHARED
MAP_ANON
MAP_NORESERVE
```

`MAP_ANON` は `vmo_fd = -1` でもよいが、kernel 内部では anonymous VMO を作る。

`MAP_PRIVATE` の COW は Phase 0 では仕様だけ決め、実装は後回しにしてよい。

## page fault

初期実装。

- unmapped access は fault
- VMA があり page が未割当なら zero-fill page を割り当てる
- protection violation は fault
- pager-backed fault は後続 phase

後続。

- pager-backed VMO
- COW
- file page cache
- lazy file mmap

## file-backed mmap

kernel は file を知らない。

file-backed mmap は FS server と libc が行う。

```text
libc mmap(file_fd)
  -> FS server に map request
  -> FS server が file-backed VMO fd を返す
  -> libc が kernel mmap(vmo_fd, ...)
```

FS server が page cache / lazy load / writeback policy を持つ。

## paddr の扱い

userland ABI に paddr を極力出さない。

例外は device / DMA debug などに限定し、通常は fd で包む。

```text
DmaBuffer fd
MmioRegion fd
DmaMapping fd
```

## 旧 page cap からの置換

| 旧概念 | 新概念 |
|---|---|
| page cap | kernel internal page allocation |
| paddr token | VMO offset / fd |
| grant page cap | fd passing VMO subrange |
| revoke page cap tree | close / selective revoke on VMO fd |
| map page | mmap VMO |
| alloc_map_pages | mmap anonymous VMO |

## Phase 0 決定事項

- VMO size limit
- VMA count limit
- initial page fault policy
- `MAP_PRIVATE` をいつ実装するか
- anonymous mmap と `vmo_create` の関係
- file-backed VMO を kernel object とするか pager-backed VMO とするか
- pkey field を VMA に持たせるか
- `brk` を libc-only にするか kernel syscall を持つか
