---
tags:
  - pachaos
  - abi
  - fd
  - phase0
---

# FD ABI Spec Draft

Phase 1 に入るための固定値は [[phase0-design-freeze]] を正とする。この文書は ABI 全体の仕様候補を説明する。

## 目的

PachaOS native ABI の中心に fd を置く。

ここでの fd は file descriptor ではなく、kernel object descriptor である。

```text
fd = process-local descriptor to a kernel object
   + per-fd rights
   + per-fd flags
```

kernel は fd table を持つが、file / path / inode / socket semantics は持たない。

## fd の性質

- fd number は process-local
- fd number は userland が任意に作っても authority にならない
- fd entry は kernel object reference を持つ
- fd entry は rights を持つ
- rights は object reference ではなく fd entry に付く
- fd は dup / transfer / close できる
- fd passing 時に rights attenuation できる
- rights escalation はできない
- close は fd entry を消し、object refcount を落とす

## 基本データ構造

実装名は仮。

```zig
pub const Fd = u32;

pub const FdEntry = struct {
    object: ObjectRef,
    rights: FdRights,
    flags: FdFlags,
};

pub const ObjectRef = struct {
    object_id: u64,
    generation: u32,
    kind: ObjectKind,
};
```

`object_id + generation` は use-after-close / stale reference の検出に使う。

## ObjectKind

初期集合。

```text
Null
Endpoint
Channel
Reply
Event
Process
Thread
AddressSpace
Vmo
Pager
Device
MmioRegion
DmaBuffer
DmaMapping
Irq
```

`File`, `Directory`, `Socket`, `Pipe`, `Tty` は kernel object にしない。

これらは userland service の protocol object であり、kernel から見ると `Channel` または `Endpoint` fd である。

## FdFlags

fd entry 自体の flags。

```text
CLOEXEC
NONBLOCK
INHERIT
PRIVATE
```

- `CLOEXEC`: exec 時に閉じる
- `NONBLOCK`: blocking operation の既定動作に影響する
- `INHERIT`: spawn / process creation 時に継承できる
- `PRIVATE`: debug / inspect などで公開しない fd

`NONBLOCK` は object semantics ではなく syscall / library の既定挙動として扱う。

## 共通 Rights

全 object に共通で使える rights。

| Right | 意味 |
|---|---|
| `INSPECT` | object type / metadata を見る |
| `DUP` | 同一 process 内で fd を複製する |
| `TRANSFER` | IPC で fd を他 process に渡す |
| `WAIT` | readiness / event を待つ |
| `POLL` | poll set に登録する |
| `SET_FLAGS` | fd flags を変更する |
| `CLOSE` | fd を閉じる |

`CLOSE` は常に許可してもよいが、rights として明示できる形にしておく。

## Object-specific Rights

### Endpoint / Channel / Reply

| Right | 意味 |
|---|---|
| `SEND` | message を送る |
| `RECV` | message を受ける |
| `CALL` | request / reply を行う |
| `ACCEPT` | connection を受ける |
| `BIND` | service name / port に bind する |
| `SIGNAL` | doorbell / event を signal する |

### VMO / Pager

| Right | 意味 |
|---|---|
| `MAP_READ` | readable mapping を作れる |
| `MAP_WRITE` | writable mapping を作れる |
| `MAP_EXEC` | executable mapping を作れる |
| `RESIZE` | VMO size を変更できる |
| `SHARE` | shared mapping / fd passing を許す |
| `PAGER_ATTACH` | pager と紐付ける |
| `PAGER_FAULT` | pager fault を処理する |

### Process / Thread / AddressSpace

| Right | 意味 |
|---|---|
| `SPAWN` | process / thread を生成する |
| `START` | suspended process / thread を開始する |
| `KILL` | 停止 / kill する |
| `DEBUG` | register / memory inspect |
| `MAP_INTO` | target address space に map する |
| `SET_CONTEXT` | initial context / TLS などを設定する |
| `SIGNAL` | interrupt / signal を送る |

### Device / MMIO / DMA / IRQ

| Right | 意味 |
|---|---|
| `QUERY` | device metadata を読む |
| `CONFIG_READ` | PCI config などを読む |
| `CONFIG_WRITE` | PCI config などを書く |
| `DERIVE_MMIO` | MMIO region fd を派生する |
| `DERIVE_DMA` | DMA buffer / mapping fd を派生する |
| `DERIVE_IRQ` | IRQ fd を派生する |
| `MMIO_MAP_READ` | MMIO readable mapping |
| `MMIO_MAP_WRITE` | MMIO writable mapping |
| `CPU_READ` | CPU が DMA buffer を読む |
| `CPU_WRITE` | CPU が DMA buffer に書く |
| `DMA_READ` | device が memory から読む |
| `DMA_WRITE` | device が memory に書く |
| `IRQ_WAIT` | IRQ を待つ |
| `IRQ_ACK` | IRQ を acknowledge する |
| `BUS_MASTER` | bus mastering を許す |

`MAP_WRITE` と `DMA_WRITE` は別権限にする。

## 基本 syscall 案

番号は未定。Phase 0 で予約範囲を決める。

```text
fd_close(fd)
fd_dup(fd, min_fd, rights_mask, flags)
fd_replace(dst_fd, src_fd, rights_mask, flags)
fd_get_info(fd, out_info)
fd_set_flags(fd, flags, mask)
fd_wait(fd, events, timeout)
fd_poll(pollfds, count, timeout)
```

`fd_dup` と `fd_replace` は rights attenuation を受け取る。

```text
new_rights <= old_rights
```

## fd passing

fd passing は IPC の一部であり、fd ABI 単体では完結しない。

fd transfer item は次の情報を持つ。

```c
struct pacha_fd_transfer {
    int fd;
    uint64_t rights;
    uint32_t flags;
};
```

受信側には新しい fd number が割り当てられる。

送信側 fd を close するかどうかは transfer flag で指定する。

```text
COPY: 送信側 fd は残る
MOVE: 送信側 fd は消える
```

## fork / spawn / exec

PachaOS native ABI は Unix fork を kernel primitive にしない。

初期方針。

- spawn は明示的に fd inheritance list を受け取る
- exec 時は `CLOEXEC` fd を閉じる
- `INHERIT` fd だけを既定継承にするか、inheritance list を必須にするかは Phase 0 で決める
- musl backend は POSIX API をこの上で表現する

## Phase 0 決定事項

- fd number の型と最大値
- rights bit width
- fd flags の最小集合
- object id / generation の形式
- fd table の初期サイズと拡張方法
- close-on-exec / spawn inheritance の既定挙動
- syscall number range
- old capability token と fd の併存期間
