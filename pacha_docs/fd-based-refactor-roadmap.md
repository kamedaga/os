# FD-based Object Microkernel Roadmap

## 目的

PachaOS を現在の capability-based microkernel から、fd-based object microkernel へ段階的に整理する。

最終目標は次の通り。

- musl libc の PachaOS native ABI backend が自然な設計で動くこと
- OS 機能へ直接 syscall ではなく `libipc` / `libcapsule` / libc 経由でアクセスできること
- 大量の capability 構造体と権限管理経路を fd table に統一すること
- page capability を ABI の主役から外し、メモリ管理を VMO / VMA / mmap に整理すること
- kernel の責務を小さくし、コード量とデバッグ対象を減らすこと
- userland は C / C ABI を主軸にし、既存の userland Zig 資産は少しずつ廃止すること
- 既存の PachaOS 専用 userland driver を kobox daemon へ置き換え、Linux kernel driver 資産を `libcapsule` 経由で扱えるようにすること

## 基本方針

PachaOS の fd は file descriptor ではなく、kernel object descriptor として定義する。

```text
fd = per-process handle to a kernel object + per-fd rights + fd flags
```

kernel は fd という概念を持つが、file という概念は持たない。

kernel に入れないもの。

- path resolution
- inode / directory / mount
- file offset
- POSIX permission
- uid / gid
- socket / pipe / tty の Unix semantics
- Linux syscall semantics

kernel が持つもの。

- fd table
- kernel object lifetime / refcount
- per-fd rights
- close / dup / transfer
- fd passing IPC
- VMO / VMA / mmap / munmap / mprotect
- process / thread / address space
- event / wait / poll
- capsule-derived hardware objects

この方針を短く表すと次になる。

```text
Everything is an fd, but not everything is a file.
```

## 権限モデル

fd は unforgeable な object reference として扱う。

安全性は capability という名前ではなく、次の設計で担保する。

- fd number は process-local で、偽造できない
- fd entry は object reference と rights を持つ
- rights は fd ごとに保持する
- dup / transfer / IPC fd passing で権限を増やせない
- fd passing 時に rights attenuation を許す
- close で reference を落とす
- revoke が必要な object だけ generation / revoked bit / child tracking を持つ
- ACL は避ける

共通 rights の例。

```text
DUP
TRANSFER
INSPECT
WAIT
POLL
```

object ごとの rights の例。

```text
Endpoint: SEND, RECV, CALL, ACCEPT
VMO: MAP_READ, MAP_WRITE, MAP_EXEC, RESIZE, SHARE
Process: SPAWN, KILL, DEBUG, MAP_INTO
Device: QUERY, CONFIG, DERIVE_MMIO, DERIVE_DMA, DERIVE_IRQ
MMIO: MAP_READ, MAP_WRITE
DMA: CPU_READ, CPU_WRITE, DMA_READ, DMA_WRITE
IRQ: WAIT, ACK
```

`DUP` と `TRANSFER` は分ける。

- `DUP`: 同一 process 内で複製できる
- `TRANSFER`: IPC で他 process へ渡せる

これにより、単に使える fd と、authority を増殖または移譲できる fd を区別できる。

## Kernel Object

初期に想定する kernel object。

- `Endpoint`
- `Channel`
- `Reply`
- `Event`
- `Process`
- `Thread`
- `AddressSpace`
- `Vmo`
- `Device`
- `MmioRegion`
- `DmaBuffer`
- `DmaMapping`
- `Irq`

FS fd、socket fd、pipe fd、tty fd は kernel object として直接持たない。

これらは userland server protocol の object であり、kernel から見ると `Channel` や `Endpoint` への fd である。

## IPC

fd-based IPC を PachaOS native ABI の中心に置く。

最低限の意味論。

```text
ipc_send(endpoint_fd, payload, attached_fds[])
ipc_recv(endpoint_fd, payload_buf, received_fds[])
ipc_call(endpoint_fd, request, attached_fds[]) -> reply + received_fds[]
```

重要なルール。

- message payload と fd transfer は atomic に成功または失敗する
- fd transfer では rights attenuation を指定できる
- fd rights escalation は禁止する
- kernel は payload の意味を知らない
- fd passing は native IPC の標準機能とする
- 同期 RPC は one-shot reply fd で表現できるようにする

例。

```text
client:
  ipc_call(fs_session_fd, OPEN, path) -> file_session_fd

server:
  recv(port_fd) receives request + reply_fd
  send(reply_fd, result + attached file_session_fd)
```

ここで `file_session_fd` は kernel にとって file ではなく channel fd である。

## libipc

kernel syscall を直接使わせず、標準 userland 経路として `libipc` を提供する。

`libipc` は普通 IPC と pkey fast IPC を同じ意味論で扱う。

```text
same semantics, different transport
```

public API の方向性。

```c
int ipc_call(
    int channel_fd,
    const void *req,
    size_t req_len,
    const int *send_fds,
    size_t send_fd_count,
    void *reply,
    size_t reply_cap,
    int *recv_fds,
    size_t *recv_fd_count
);
```

使い分け。

- normal IPC
  - 初回接続
  - fd passing
  - 権限移譲
  - protocol negotiation
  - debug / trace
  - pkey 非対応環境

- pkey fast IPC
  - 高頻度 channel
  - data plane
  - fd passing を伴わない request / completion
  - block / net / console / GUI / audio など

設計原則。

```text
fd = authority / lifetime / transfer
pkey = fast dataplane access control
```

pkey は authority ではない。authority は fd rights である。

x86 PKU / PKRU は user 命令で切り替え可能なので、強い security boundary として扱わない。pkey は trusted runtime 内の高速 phase control として使う。

## Memory

page capability を ABI の主役から外す。

page は kernel 内部の実装単位であり、userland に見せる抽象ではない。

userland に見せる抽象。

```text
VMO = memory object / backing store
VMA = process address-space mapping
mmap = VMO fd を address space に map する操作
```

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

fd rights と VMA protection の関係。

```text
VMA prot <= VMO fd rights
```

例。

```text
VMO fd rights: MAP_READ | MAP_WRITE
mmap PROT_READ          OK
mmap PROT_READ|WRITE    OK
mmap PROT_EXEC          NG
```

file-backed mmap は kernel に file を入れず、FS server が VMO fd を返す。

```text
libc mmap(file_fd)
  -> FS server に request
  -> FS server が file-backed VMO fd を返す
  -> libc が kernel mmap(vmo_fd, ...)
```

初期実装では demand paging や page cache まで入れず、次を優先する。

- anonymous VMO
- eager allocation または fault-time zero-fill
- mmap / munmap / mprotect
- shared VMO mapping
- fd passing による shared memory

後続で検討するもの。

- pager-backed VMO
- copy-on-write
- file page cache
- lazy file mmap

## Capsule

capsule も fd-based object model に統一する。

`capsule_fd` は hardware / device authority を表す kernel object fd とする。

想定 object。

```text
Device fd
MmioRegion fd
DmaBuffer fd
DmaMapping fd
Irq fd
```

DMA と MMIO は通常の VMO と同一視しすぎない。

- CPU writable と device writable は別権限
- `MAP_WRITE` と `DMA_WRITE` は別権限
- MMIO は範囲、cache 属性、write 権限を kernel が検証する

`libcapsule` は driver / kobox backend / hardware-facing userland の標準入口とする。

API の方向性。

```c
int capsule_query(int dev_fd, struct capsule_info *out);
int capsule_pci_config_read(int dev_fd, ...);
int capsule_pci_config_write(int dev_fd, ...);
int capsule_derive_mmio(int dev_fd, unsigned bar, int *mmio_fd);
int capsule_derive_dma_buffer(int dev_fd, size_t size, unsigned flags, int *dma_fd);
int capsule_map_mmio(int mmio_fd, void **addr, size_t *len);
int capsule_irq_wait(int irq_fd, uint64_t timeout);
```

driver は syscall を直接叩かず、`libcapsule` 経由で device authority を扱う。

## libc / Userland Layer

fd-based native ABI により、C library layer を自然に構成する。

```text
app
  -> musl libc PachaOS backend
  -> libpacha
  -> libipc
  -> libcapsule
  -> fd-based kernel ABI
  -> userland servers
```

`libpacha` は raw native ABI veneer とする。

`musl libc` backend は次を fd ベースで実装する。

- `open`
- `read`
- `write`
- `close`
- `dup`
- `poll`
- `mmap`
- `munmap`
- `mprotect`
- `posix_spawn`
- `exec`
- `pipe`
- `socket` facade

kernel fd table は統一する。

kernel-native object も userland service session も同じ fd table に入る。ただし service object の意味は libc / service client library が解釈する。

userland 実装の主軸は C に寄せる。

- PachaOS native ABI の public surface は C ABI として安定させる
- `libpacha`, `libipc`, `libcapsule` は C で実装し、C library として提供する
- musl backend から自然に呼べる header / ABI を優先する
- 既存の `userland/programs/*.zig` と Zig ABI helper は、対応する C library / C service / generated header へ段階移行する
- kernel Zig と userland C の境界を明確にし、userland 側で Zig-only な ABI 依存を増やさない

## kobox Driver Daemons

kobox は PachaOS の driver daemon 基盤として扱う。

README の kobox セクションにある通り、kobox は Linux kernel driver (`.ko`) を userland process として直接実行する runtime である。fd-based refactor 後は、既存の PachaOS 専用 driver を少しずつ kobox daemon に置き換える。

目標。

- NVMe / USB / HID などは、可能な範囲で kobox daemon として扱う
- PachaOS 側の hardware authority は `Device fd`, `MmioRegion fd`, `DmaBuffer fd`, `DmaMapping fd`, `Irq fd` として渡す
- kobox PachaOS backend は syscall を直接叩かず、`libcapsule` を使う
- kobox backend が必要とする MMIO / DMA / IRQ / PCI config 操作を `libcapsule` API に反映する
- driver daemon と他 service の通信は `libipc` を使う
- `libipc` / `libcapsule` は C 実装なので、kobox backend や daemon から直接 link できる
- 既存 driver code を kernel に寄せず、userland daemon と fd passing で構成する

これにより、PachaOS 固有 driver を増やすのではなく、Linux driver 資産を userland daemon として再利用する方向へ寄せる。

## Migration Phases

### Phase 0: Design Freeze

- fd の意味を文書化する
- rights bit の分類を決める
- object type の初期集合を決める
- old capability API との対応表を作る
- syscall numbering 方針を決める
- userland C ABI / header 方針を決める
- kobox backend が `libcapsule` に要求する最小 API を README と既存起動経路から洗い出す

Phase 0 の作業入口は [[phase0-checklist]]。

関連する仕様候補。

- [[phase0-design-freeze]]
- [[fd-abi-spec]]
- [[ipc-abi-spec]]
- [[memory-abi-spec]]
- [[capsule-fd-spec]]
- [[capability-to-fd-migration-map]]

### Phase 1: FD Table Core

- process-local fd table を導入する
- `FdEntry`, `FdRights`, `FdFlags`, `KernelObject` を実装する
- close / dup / transfer の基本操作を入れる
- object refcount と lifetime を統一する

この段階では既存 capability path と併存してよい。

実装計画は [[phase1-fd-table-core-plan]] に固定する。

### Phase 2: Memory Decoupling

- `UserAddressSpace` を capability namespace から切り離す
- page allocator と capability install/remove を分離する
- page cap を memory fast path から外す
- anonymous VMA / VMO / mmap を導入する
- `alloc page -> cap install -> map` の流れを廃止する

ここが最優先の大きな山である。

### Phase 3: VMO fd

- `VmObjectCapability` を `Vmo` fd に移す
- VMO fd rights を定義する
- `mmap(vmo_fd)` を native ABI にする
- shared memory を fd passing で扱う
- file-backed mmap のための userland FS server protocol を設計する

実装メモは [[phase3-vmo-fd]] に置く。

旧 VM object cap syscall は互換実装を残さず invalid にする。fd passing が入るまでは、別 process userland server からの exec VMO 共有は未完成でよい。

### Phase 4: FD Passing IPC

- `Endpoint` / `Channel` / `Reply` fd を実装する
- fd passing を IPC の標準機能にする
- rights attenuation を実装する
- `libipc` を提供する
- normal IPC を安定させる

実装メモは [[phase4-fd-passing-ipc]] に置く。

### Phase 5: Fast IPC

- shared VMO ring を使う fast IPC channel を設計する
- pkey support を opt-in backend として入れる
- normal IPC fallback を常に維持する
- fd passing は control plane、fast IPC は data plane として分離する

Phase 5 の pkey 方針は [Phase 5 fast IPC / pkey threat model](./phase5-fast-ipc-pkey-threat-model.md) に固定する。

pkey は untrusted IPC の主境界ではない。untrusted peer 間 IPC は fd rights / VMO fd / VMA/PTE permission で成立させ、pkey は local fast-path guard としてのみ使う。

### Phase 6: Capsule fd

- capsule object を fd-based model に移す
- `Device` / `MmioRegion` / `DmaBuffer` / `DmaMapping` / `Irq` fd を作る
- `libcapsule` を提供する
- userland driver と kobox backend を `libcapsule` 経由に寄せる
- kobox を driver daemon として扱えるよう、device fd bootstrap / daemon supervision / IPC protocol を整理する

### Phase 7: musl Native Backend

- Linux ABI server ではなく PachaOS native ABI で musl を動かす
- 最初の smoke target を決める
  - hello world
  - stdio
  - malloc / mmap
  - simple file read via FS server
  - poll
  - spawn / exec
- libc から syscall 直叩きを減らし、`libipc` / `libcapsule` / `libpacha` に集約する
- userland Zig で提供していた ABI helper / service code を C library / C service に置き換える

### Phase 8: Old Capability Removal

- page capability table を削除する
- VM object cap table を削除する
- IPC buffer cap を fd object に統合する
- device / queue / command cap を capsule fd に統合する
- PachaOS 専用 driver のうち kobox daemon で置換できるものを削除または compatibility に隔離する
- userland Zig 資産を native C ABI / C service へ移行し、不要になった Zig app build path を削除する
- old syscall を削除または compatibility shim に隔離する
- README / docs / tests を fd-based 設計へ更新する

## 成功条件

段階的な成功条件。

- page cap なしで anonymous mmap が動く
- VMO fd を fd passing して shared memory が動く
- `libipc` 経由で request / reply / fd passing が動く
- `libcapsule` 経由で MMIO / DMA / IRQ を扱える
- kobox PachaOS backend が `libcapsule` 経由で device fd を扱える
- 既存 userland driver の一部が kobox daemon に置き換わる
- Linux ABI server なしで musl hello world が動く
- libc の `mmap`, `read`, `write`, `poll`, `close`, `dup` が native ABI 上で動く
- userland Zig helper なしで主要 native ABI smoke が動く
- 旧 capability syscall と構造体を削除できる
- kernel の capability 系コード行数が明確に減る

## 注意点

移行中はコード量が一時的に増える。

したがって短期目標を「すぐ減らす」ではなく、「削除可能な境界を作る」に置く。

kernel 側へ新機能を足す時は、次を確認する。

- userland library で実現できないか
- kernel object として本当に必要か
- file / Unix semantics が kernel に侵入していないか
- fd rights で authority を表現できるか
- page / paddr を userland ABI に出していないか

PachaOS の新しい説明は次の形を目指す。

```text
PachaOS is an fd-based object microkernel.
The kernel provides fd-addressed objects, fd passing IPC, VMO mapping,
scheduling, and hardware isolation.
Unix and Linux semantics live in userland.
```
