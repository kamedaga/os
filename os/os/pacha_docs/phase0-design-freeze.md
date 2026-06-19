---
tags:
  - pachaos
  - phase0
  - fd
  - decision
---

# Phase 0 Design Freeze

## Status

Phase 1 のための固定設計。

この文書は fd table core を実装するための決定事項をまとめる。Phase 1 中に変更が必要になった場合は、この文書を更新してから実装を変える。

## Scope

Phase 0 で固定するもの。

- fd core の型と制限
- fd rights / flags の表現
- object reference と lifetime の最小設計
- process-local fd table の配置
- close / dup / transfer の意味論
- Phase 1 の実装境界
- 既存 capability path との併存方針

Phase 0 で固定しないもの。

- VMO fd の完全な実装
- fd passing IPC の syscall 実装
- capsule fd の完全な実装
- pkey fast IPC
- musl native backend
- userland Zig 資産の削除
- kobox daemon の実装

## Core Decisions

### fd number

- public C ABI type: `int`
- kernel internal type: `u32`
- valid fd range in Phase 1: `0..255`
- invalid fd in C library: `-1`
- kernel rejects fd values `>= 256`

理由。

- 現在の Linux ABI server は `LINUX_FD_MAX = 256` を使っている
- Phase 1 では dynamic fd table growth を入れず、fd core の correctness を優先する
- musl / C API から自然に扱える

将来、fd table growth を入れる場合も public type は変えない。

### fd table

fd table は process-local とする。

Phase 1 の構造。

```zig
pub const fd_table_entries: usize = 256;

pub const FdTable = struct {
    entries: [fd_table_entries]FdEntry = [_]FdEntry{.{}} ** fd_table_entries,
};
```

`KernelState` には既存の process table と同じ形で持つ。

```zig
fd_tables: [process_count]FdTable
fd_tables_extra: []FdTable
```

`ensureProcessCapacity` では `fd_tables_extra` も他の process-local table と同じタイミングで拡張する。

device principal 用 fd table は Phase 1 では作らない。device authority は後続の capsule fd phase で扱う。

### fd entry

Phase 1 の fd entry。

```zig
pub const FdEntry = struct {
    object: KernelObjectRef = .{},
    rights: FdRights = .{},
    flags: FdFlags = .{},
};
```

empty entry は `object.kind == .none` で表す。

rights は object ではなく fd entry に付く。

### fd flags

Phase 1 の flags は `u32` bitset とする。

```text
CLOEXEC
NONBLOCK
INHERIT
PRIVATE
```

Phase 1 で意味を実装するもの。

- `CLOEXEC`: entry に保持する
- `NONBLOCK`: entry に保持する
- `INHERIT`: entry に保持する
- `PRIVATE`: entry に保持する

Phase 1 では exec / spawn / poll と接続しない。保持と dup / transfer 時の伝搬だけを実装する。

### fd rights

Phase 1 の rights は `u64` bitset とする。

共通 rights。

```text
INSPECT
DUP
TRANSFER
WAIT
POLL
SET_FLAGS
CLOSE
```

Phase 1 で実際に enforcement するもの。

- `DUP`: `dupFd` に必要
- `TRANSFER`: `transferFd` に必要
- `SET_FLAGS`: `setFdFlags` に必要

`CLOSE` は Phase 1 では常に許可する。fd は authority だが、fd を捨てることは常に可能にする。

object-specific rights は bit allocation だけ予約し、Phase 1 では enforcement しない。

### rights attenuation

dup / transfer / replace では必ず rights attenuation を行う。

```text
new_rights <= source_rights
```

違反した場合は `KernelError.InvalidState` を返す。

rights が `0` になる fd は許可する。これは sealed / placeholder 的な fd を将来作る余地を残すため。

### object reference

Phase 1 の object reference。

```zig
pub const KernelObjectRef = struct {
    kind: KernelObjectKind = .none,
    index: u32 = 0,
    generation: u32 = 0,
};
```

`kind == .none` は null reference。

`index + generation` で stale reference を検出する。

### object kind

Phase 1 の initial kind。

```text
none
process
endpoint_compat
vmo_compat
capsule_compat
event
```

意味。

- `process`: process descriptor への fd
- `endpoint_compat`: 既存 endpoint capability path と併存するための placeholder
- `vmo_compat`: 既存 VM object capability path と併存するための placeholder
- `capsule_compat`: 既存 capsule token path と併存するための placeholder
- `event`: wait/poll 系の後続実装用

Phase 1 では object-specific operation は実装しない。

### object table

Phase 1 では `KernelState` に global object table を持つ。

```zig
pub const max_fd_objects: usize = 4096;

pub const KernelObjectSlot = struct {
    kind: KernelObjectKind = .none,
    generation: u32 = 1,
    ref_count: u32 = 0,
    payload: KernelObjectPayload = .none,
};
```

object allocation は空き slot を線形探索してよい。

Phase 1 の目的は performance ではなく lifetime invariant の固定である。

### refcount

object lifetime は object table の `ref_count` で管理する。

ルール。

- fd install で `ref_count += 1`
- fd close で `ref_count -= 1`
- dup 成功で `ref_count += 1`
- transfer COPY 成功で `ref_count += 1`
- transfer MOVE 成功では refcount は変えない
- replace は destination close と source install を atomic に扱う
- `ref_count == 0` になった object は payload cleanup 後に free
- free 時に `generation += 1`

generation が `0` に wrap した場合は `1` に進める。

### transfer

Phase 1 の transfer は KernelState helper として実装する。

```text
transferFd(from, to, fd, min_fd, rights, flags, mode)
```

mode。

```text
copy
move
```

ルール。

- source fd に `TRANSFER` right が必要
- requested rights は source rights の subset
- destination は `min_fd` 以上の空き fd を使う
- COPY は source fd を残す
- MOVE は成功時に source fd を empty にする
- 失敗時に source / destination を変更しない

IPC message との接続は Phase 4 で行う。

### close-on-exec / inheritance

Phase 1 では fd flags として保持するだけ。

固定方針。

- native spawn は明示的な inheritance list を使う
- `INHERIT` は library default policy 用の metadata として残す
- `CLOEXEC` は musl / exec layer が fd table を整理するために使う
- kernel は Phase 1 で exec inheritance policy を実装しない

### syscall range

fd-based native ABI は新しい syscall range を使う。

Phase 0 の予約。

```text
0x100..0x13f  fd core
0x140..0x17f  ipc
0x180..0x1bf  memory / vmo
0x1c0..0x1ff  process / thread
0x200..0x23f  capsule fd
0x240..0x27f  debug / inspect
```

Phase 1 では syscall 番号の定数を追加してよいが、最初の実装は KernelState helper と unit test を優先する。

既存 syscall をこの range に移動しない。old capability ABI は compatibility として残す。

## Phase 1 Boundary

Phase 1 でやる。

- `FdEntry`, `FdRights`, `FdFlags`, `KernelObjectRef` を追加する
- `KernelObjectSlot` と global object table を追加する
- process-local `FdTable` を `KernelState` に追加する
- process capacity growth に fd table extra を追加する
- close / dup / replace / transfer helper を追加する
- process teardown で fd table を release する
- unit tests を追加する

Phase 1 でやらない。

- VMO fd の実 memory mapping
- IPC syscall への fd passing 統合
- capsule token の fd 置換
- public libc / libipc / libcapsule 実装
- old capability syscall 削除

## Verification

Phase 1 の最低テスト。

- fd install allocates lowest available fd
- fd close drops entry and decrements object refcount
- fd dup requires `DUP`
- fd dup rejects rights escalation
- fd transfer COPY requires `TRANSFER`
- fd transfer COPY increments refcount
- fd transfer MOVE removes source without changing refcount
- fd replace closes destination exactly once
- process runtime reset releases all fds
- ensureProcessCapacity preserves fd tables across growth

## Related

- [[fd-abi-spec]]
- [[phase1-fd-table-core-plan]]
- [[capability-to-fd-migration-map]]
