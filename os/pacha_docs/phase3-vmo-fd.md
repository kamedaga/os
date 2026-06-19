---
tags:
  - pachaos
  - phase3
  - vmo
  - fd
  - memory
---

# Phase 3 VMO fd

Phase 3 の目的は、`VmObjectCapability` を ABI の主役から外し、VMO を fd object として扱うこと。

互換維持は目標にしない。既存の VM object cap syscall は invalid を返すだけにして、旧経路を修理しない。

## 目標

- `VmObjectCapability` / VM object cap table を kernel から外す
- VMO fd rights を定義する
- `mmap(vmo_fd)` を native ABI にする
- exec image / file-backed mmap の結果を VMO fd として扱う
- shared memory は VMO fd passing を前提にする
- userland FS server protocol を file-backed VMO fd に寄せる

## ABI

Phase 3 で追加する fd range syscall。

| syscall | number | 意味 |
|---|---:|---|
| `fd_close` | `0x100` | fd を閉じる |
| `fd_dup` | `0x101` | rights attenuation 付き dup |
| `fd_get_info` | `0x103` | kind / rights / flags / size を読む |
| `fd_set_flags` | `0x104` | fd flags を変更する |
| `vmo_create` | `0x107` | anonymous VMO fd を作る |
| `vmo_from_current_pages` | `0x108` | current mapping の内容を anonymous VMO fd にコピーする |
| `mmap` | `0x109` | VMO fd を address space に map する |
| `munmap` | `0x10A` | VMA を外す |

`vmo_from_current_pages` は旧 cap 互換ではない。既存 mapping の page cap ownership を移すのではなく、内容を新しい VMO にコピーする。

## VMO fd rights

VMO mapping に必要な権限。

| right | 意味 |
|---|---|
| `MAP_READ` | readable mapping を作れる |
| `MAP_WRITE` | writable mapping を作れる |
| `MAP_EXEC` | executable mapping を作れる |
| `RESIZE` | VMO size 変更 |
| `SHARE` | shared mapping / fd passing を許す |
| `PAGER_ATTACH` | userland pager 接続 |
| `PAGER_FAULT` | pager fault 処理 |

`mmap` の `prot` は fd rights の subset でなければならない。

## fd number policy

現在の syscall error は小さい正数で返るため、syscall が直接返す dynamic fd は `16` 以降にする。

これは恒久仕様ではなく、Phase4 以降で errno/negative error 形式を整理するまでの衝突回避である。

## kernel 実装

Phase 3 で kernel に残すもの。

- `KernelObjectKind.vmo`
- process-local fd table
- native VMO table
- native VMA table
- VMO backing page store
- page fault から VMA/VMO を引く経路

Phase 3 で kernel から外すもの。

- `VmObjectCapability`
- `VmObjectRights`
- VM object cap table
- VM object cap chunk pool
- VM object token encode/decode
- VM object grant/drop/revoke 実装
- process builder / ABI trap の VM object mapping 互換経路

## userland FS protocol

`open_exec` の result は VM object token ではなく VMO fd として扱う。

Phase 3 時点では fd passing がまだ標準 IPC に入っていないため、別 process の FS server から client へ fd を正しく渡す部分は Phase 4 の対象になる。

そのため Phase 3 の FS protocol は次の中間状態にする。

- response の `result_token` 欄は exec VMO fd として扱う
- 旧 VM object token tag は見ない
- `syscall_grant_vm_object` は使わない
- fd passing が入るまで、別 process server の exec VMO 共有は未完成として扱う

## file-backed mmap

kernel は file を知らない。

file-backed mmap は userland FS server と libc が作る。

```text
libc mmap(file_fd)
  -> libipc で FS server に map request
  -> FS server が VMO fd を返す
  -> libc が mmap(vmo_fd, offset, prot, flags) を呼ぶ
```

Phase 3 では protocol の意味を VMO fd に寄せる。実際の fd passing は Phase 4、pager-backed / lazy file mmap は後続で扱う。

## 破壊的移行対象

次の userland は旧 VM object token / grant 前提が残っているため、Phase3 後は修理ではなく C library / libipc / fd passing の設計に合わせて置き換える。

- `userland/seed2_boot`
- `userland/seed2_root`
- `userland/exec`
- `userland/linux_abi_server`
- `userland/rootfs/vfs`

互換 shim を足すより、Phase4 で fd passing を入れてから、VMO fd を渡す経路へ移す。

## 完了条件

- kernel の VM object cap table が消えている
- 旧 VM object syscalls が invalid になっている
- `vmo_create` / `vmo_from_current_pages` / `mmap` / `munmap` が fd ABI に入っている
- VMO mapping は fd rights で検査される
- rootfs / FS client の exec result が VM object token tag を見ない
- docs に未移行 userland と Phase4 依存を明記する
