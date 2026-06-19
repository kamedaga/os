---
tags:
  - pachaos
  - phase1
  - fd
  - implementation-plan
---

# Phase 1 FD Table Core Plan

## Goal

process-local fd table を導入し、fd entry と kernel object lifetime を統一する。

Phase 1 では既存 capability path と併存する。page cap、VM object cap、capsule token、endpoint capability は削除しない。

## Authoritative Design

Phase 1 の固定値は [[phase0-design-freeze]] に従う。

この plan は実装順序と touched file を示す。

## Expected Files

追加または変更候補。

```text
kernel/src/kernel.zig
kernel/src/syscall/numbers.zig
kernel/abi/fd_abi.zig
kernel/abi/kernel_abi_root.zig
userland/programs/abi/fd_abi.zig
userland/programs/abi/abi_root.zig
tests/kernel_state.zig
```

Phase 1 の最初の commit では syscall dispatch を入れなくてもよい。`kernel/abi/fd_abi.zig` と `numbers.zig` の定数追加は、C header 生成や後続 `libpacha` のために予約する。

## Implementation Order

### 1. Types

`kernel/src/kernel.zig` に fd core type を追加する。

```zig
pub const Fd = u32;
pub const fd_table_entries: usize = 256;
pub const max_fd_objects: usize = 4096;

pub const FdRights = packed struct(u64) { ... };
pub const FdFlags = packed struct(u32) { ... };
pub const KernelObjectKind = enum(u16) { ... };
pub const KernelObjectRef = struct { ... };
pub const FdEntry = struct { ... };
pub const FdTable = struct { ... };
pub const KernelObjectSlot = struct { ... };
```

Phase 1 では separate file に切り出さない。既存の `KernelState` と tests が大きく依存しているため、まず `kernel.zig` 内で安定させる。

後続 phase で `kernel/src/fd.zig` などへ分割してよい。

### 2. KernelState storage

`KernelState` に次を追加する。

```zig
fd_tables: [process_count]FdTable
fd_tables_extra: []FdTable
fd_objects: [max_fd_objects]KernelObjectSlot
next_fd_object_scan: usize
```

empty extra slice も追加する。

```zig
var empty_fd_tables_extra: [0]FdTable = .{};
```

`ensureProcessCapacity` では `FdTable` extra を既存 process-local table と同じように確保、copy、初期化する。

### 3. Accessors

process index 用 accessor を追加する。

```zig
fn fdTableForProcessIndex(self: *KernelState, index: usize) *FdTable
fn fdTableForProcessIndexConst(self: *const KernelState, index: usize) *const FdTable
pub fn getFdTable(self: *KernelState, principal: PrincipalId) *FdTable
pub fn getFdTableConst(self: *const KernelState, principal: PrincipalId) *const FdTable
```

device principal は Phase 1 では reject する。

### 4. Object table helpers

必要 helper。

```zig
fn allocKernelObject(self: *KernelState, kind: KernelObjectKind, payload: KernelObjectPayload) KernelError!KernelObjectRef
fn retainKernelObject(self: *KernelState, ref: KernelObjectRef) KernelError!void
fn releaseKernelObject(self: *KernelState, ref: KernelObjectRef) void
fn kernelObjectSlot(self: *KernelState, ref: KernelObjectRef) ?*KernelObjectSlot
fn kernelObjectSlotConst(self: *const KernelState, ref: KernelObjectRef) ?*const KernelObjectSlot
```

invariant。

- `retain` は stale ref を reject
- `release` は stale ref を ignore してよいが、test では発生させない
- `release` で refcount 0 なら payload cleanup と generation bump

### 5. fd table helpers

必要 helper。

```zig
pub fn installFd(self: *KernelState, owner: PrincipalId, object: KernelObjectRef, rights: FdRights, flags: FdFlags, min_fd: Fd) KernelError!Fd
pub fn closeFd(self: *KernelState, owner: PrincipalId, fd: Fd) KernelError!void
pub fn dupFd(self: *KernelState, owner: PrincipalId, fd: Fd, min_fd: Fd, rights: FdRights, flags: FdFlags) KernelError!Fd
pub fn replaceFd(self: *KernelState, owner: PrincipalId, dst_fd: Fd, src_fd: Fd, rights: FdRights, flags: FdFlags) KernelError!void
pub fn transferFd(self: *KernelState, from: PrincipalId, to: PrincipalId, fd: Fd, min_fd: Fd, rights: FdRights, flags: FdFlags, mode: FdTransferMode) KernelError!Fd
```

Phase 1 では `installFd` は object ref を受け取る。object creation helper は tests と later phases が使う。

### 6. process cleanup

`resetProcessRuntimeTables` または process teardown path で fd table を release する。

要求。

- process reuse 時に fd leak がない
- process exit / remove で object refcount が落ちる
- existing cap release path には干渉しない

### 7. ABI constants

`kernel/abi/fd_abi.zig` を追加する。

最初の定数。

```zig
pub const fd_table_entries: u32 = 256;
pub const syscall_fd_first: u64 = 0x100;
pub const syscall_fd_close: u64 = 0x100;
pub const syscall_fd_dup: u64 = 0x101;
pub const syscall_fd_replace: u64 = 0x102;
pub const syscall_fd_get_info: u64 = 0x103;
pub const syscall_fd_set_flags: u64 = 0x104;
pub const syscall_fd_wait: u64 = 0x105;
pub const syscall_fd_poll: u64 = 0x106;
pub const syscall_fd_last: u64 = syscall_fd_poll;
```

Phase 1 では syscall dispatch 実装は任意。実装する場合も fd core helper を薄く呼ぶだけにする。

### 8. Tests

`tests/kernel_state.zig` に fd core tests を追加する。

優先テスト。

- install fd uses lowest available slot
- close releases object
- dup requires `DUP`
- dup attenuates rights
- dup rejects escalation
- transfer COPY requires `TRANSFER`
- transfer COPY increments refcount
- transfer MOVE empties source
- replace releases destination
- reset process runtime releases fd table
- ensureProcessCapacity copies fd table extra

## Coexistence Policy

Phase 1 では既存 API はそのまま残す。

- `cap_tables`
- `endpoint_tables`
- `ipc_buffer_tables`
- `vm_object_tables`
- `capsules`
- queue / command / iommu cap tables

fd table はこれらを直接置き換えない。後続 phase で compat object を通して順に移す。

## Locking

既存 syscall path は `KernelStateSpinLock` と `syscall_lock_policy` を持つ。

Phase 1 helper は `KernelState` mutation として実装し、呼び出し側が既存 lock policy に従う。

helper 内で独自 spinlock は持たない。

## Error Mapping

Kernel helper は既存 `KernelError` を使う。

推奨 mapping。

| 状態 | KernelError |
|---|---|
| invalid fd | `InvalidState` |
| stale object ref | `InvalidState` |
| fd table full | `TableFull` |
| object table full | `TableFull` |
| rights escalation | `InvalidState` |
| inactive process | `InvalidState` |

public syscall error number mapping は syscall dispatch を入れる phase で決める。

## Review Checklist

実装レビュー時に見ること。

- fd rights が object ではなく fd entry にある
- rights attenuation が dup / transfer / replace で必ず行われている
- MOVE transfer が refcount を二重に増減していない
- replace が destination object を leak しない
- process cleanup が fd table を全 release する
- existing capability tests が壊れていない
- Phase 1 で file semantics が入っていない

## Verification Commands

ドキュメント上の推奨。

```powershell
zig build test
```

または pacgo 経由で kernel tests が定義されている場合はそれを使う。

Windows 側 Zig が child compiler spawn に失敗する場合は、AGENTS.md の方針通り WSL 側 toolchain で実行する。
