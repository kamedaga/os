# filed VFS Design

## 目的

`koboxd` まで NVMe と Linux `.ko` loader/backend が到達したので、次の主対象は rootfs の顔になる VFS である。

ただし VFS を `koboxd` に入れない。`koboxd` は Linux driver `.ko` runtime / device backend / block provider に集中させる。VFS は独立 daemon の `filed` が担当する。

VFS は単なる `open/read` smoke ではなく、PachaOS の userland が日常的に触るファイル API の基盤になる。そのため、既存 Unix / BSD の堅牢なファイル意味論と、Linux ext4 / 将来の btrfs backend が要求する引数・状態・呼び出し順の両方を満たす必要がある。

この文書では、`filed` VFS、PachaOS native `libvfs`、musl/POSIX layer、trusted pkey IPC backend、`koboxd` block provider との境界を固定する。

## Daemon 分離

恒久構造は次のようにする。

```text
storage_boot
  -> seed0root
    -> koboxd  : .ko loader / capsule device backend / NVMe block driver daemon
    -> filed   : VFS / vnode / mount / file handle server
```

`koboxd` の責務。

- capsule device fd を受け取る
- PCI config / BAR / DMA / IRQ を `libcapsule` 経由で扱う
- NVMe backend を作る
- `nvme-auth.ko`, `nvme-core.ko`, `nvme.ko` を load/init する
- block device service endpoint を公開する
- Linux driver `.ko` runtime に集中する

`filed` の責務。

- rootfs block device endpoint を `koboxd` から受け取る
- ext4 / btrfs backend を mount する
- vnode / mount / open file description / handle table / path walk を持つ
- `libvfs` / musl から呼ばれる file service endpoint を公開する
- filesystem cache と writeback policy を所有する

この分離により、`koboxd` が VFS server まで兼任して肥大化することを避ける。将来 `netd`, `inputd`, `displayd` などを増やす場合も、`koboxd` は device/driver runtime、`filed` は file namespace server という役割が保たれる。

## 基本方針

- kernel に VFS / FS / block file semantics を入れない
- `koboxd` は block provider とする
- `filed` を VFS server とし、filesystem `.ko` backend と namespace を所有させる
- VFS core は PachaOS の vnode model として設計する
- ext4 / btrfs は VFS backend として扱い、Linux の `inode` / `dentry` / `file` 構造体を core に漏らさない
- PachaOS native daemon 向けに `libvfs` を提供する
- POSIX 互換 API は musl layer で提供し、`libvfs` を POSIX API そのものにしない
- `filed` との高速 IPC は trusted 専用の pkey shared-memory backend を使えるようにする
- normal IPC / fd passing は control plane と fallback として維持する

## 対応 filesystem

最初の対応対象は ext4 とする。

将来対応対象として btrfs を想定する。btrfs は ext4 より object identity、subvolume、snapshot、checksum、tree operation の性質が異なるため、VFS core は ext4 固有の inode number 前提に寄せすぎてはいけない。

対応順。

1. ext4 rootfs mount
2. ext4 regular file read/write
3. ext4 directory operation
4. ext4 rename/link/unlink/fsync
5. btrfs backend 設計
6. btrfs read/write smoke

## vnode model

PachaOS VFS core は vnode ベースにする。

### Vnode

`Vnode` は filesystem object を表す。

保持するもの。

- object type: regular file, directory, symlink, device, fifo など
- stable identity
- refcount
- cached metadata
- parent mount
- backend private pointer
- vnode ops table

`Vnode` は open file description ではない。file offset や open flags は `Vfile` が持つ。

### Vnode identity

Vnode identity は `(mount_id, backend_object_id)` とする。

ext4 backend の `backend_object_id` 候補。

- inode number
- inode generation

btrfs backend の `backend_object_id` 候補。

- root id
- object id
- generation

VFS core はこれらの詳細を解釈しない。backend が compare / hash 用の stable id を返す。

### Vmount

`Vmount` は mounted filesystem instance を表す。

保持するもの。

- mount id
- filesystem type
- root vnode
- block device binding
- mount flags
- backend superblock/private state

cross-mount operation は VFS core が判断する。`rename` の cross-mount は `EXDEV` 相当を返す。

### Vfile

`Vfile` は open file description を表す。

保持するもの。

- vnode pointer
- current offset
- open flags
- rights
- backend file/private state
- refcount

`dup` 相当では `Vfile` を共有し、file offset も共有する。`open` し直した場合は別の `Vfile` になる。

### VfsHandle

`VfsHandle` は `filed` IPC から見える opaque handle である。

保持するもの。

- handle id
- points to `Vfile`, directory cursor, mount, or control object
- per-handle rights
- close / transfer / inherit policy

client に vnode pointer や backend pointer は見せない。

## Linux FS backend 境界

ext4 / btrfs backend は Linux `.ko` を使う。

ただし PachaOS VFS core は Linux VFS の clone にはしない。Linux `.ko` が要求する `struct inode`, `struct dentry`, `struct file`, `address_space`, `kiocb`, `iov_iter` などは backend adapter が作る。

責務分離。

| layer | 責務 |
| --- | --- |
| VFS core | vnode, mount, path walk, handle lifetime, rights, common semantics |
| FS backend adapter | VnodeOps を Linux `.ko` 呼び出しに変換 |
| ext4/btrfs `.ko` | filesystem implementation |
| block client | `koboxd` block endpoint への read/write |
| block provider | `koboxd` NVMe block service |

Linux adapter に閉じ込めるもの。

- fake / shim Linux object layout
- `inode` / `dentry` / `file` allocation
- `kiocb` / `iov_iter` construction
- Linux errno/status conversion
- Linux lock/wait/work shim
- ext4/btrfs 固有の mount/probe state

VFS core に入れてはいけないもの。

- ext4 extent layout
- btrfs tree layout
- Linux `struct file` offset
- Linux `dentry` cache semantics
- Linux page cache assumptions

## VFS API

`libvfs` は PachaOS native API であり、POSIX API ではない。

最初から想定する操作。

- `vfs_openat(dir, path, flags, mode, rights) -> handle`
- `vfs_close(handle)`
- `vfs_pread(file, offset, buffer, length) -> bytes`
- `vfs_pwrite(file, offset, buffer, length) -> bytes`
- `vfs_read(file, buffer, length) -> bytes`
- `vfs_write(file, buffer, length) -> bytes`
- `vfs_statx(handle or path) -> metadata`
- `vfs_getdents(dir, cursor, buffer, length) -> bytes`
- `vfs_fsync(file, flags)`
- `vfs_mkdirat(dir, path, mode)`
- `vfs_unlinkat(dir, path, flags)`
- `vfs_renameat(old_dir, old_path, new_dir, new_path, flags)`
- `vfs_linkat(old_dir, old_path, new_dir, new_path, flags)`
- `vfs_symlinkat(target, dir, path)`
- `vfs_readlinkat(dir, path, buffer, length)`

`pread` / `pwrite` を primitive とし、`read` / `write` は `Vfile` の current offset を更新する wrapper とする。

## Path walk

`openat` を基本 primitive にする。

absolute path は process/root namespace の root handle から解決する。relative path は dir handle から解決する。

最初から仕様化するもの。

- `.` の処理
- `..` の処理
- root escape の禁止
- mount crossing
- symlink follow / nofollow
- symlink depth limit
- trailing slash
- empty path
- directory-only open
- create / exclusive create

path walk は VFS core の責務である。backend は single component lookup を提供する。

## Unix/BSD 的な file semantics

VFS は既存エコシステムを直接使えない分、挙動の堅牢さを仕様として持つ。

### unlink-open lifetime

unlink 済みでも open されている file は `Vfile` / `Vnode` refcount により生存する。directory entry が消えても、open file description は read/write/fsync/close 可能である。

### rename atomicity

同一 mount 内の `rename` は backend が atomic に処理できる場合のみ成功する。

cross-mount rename は失敗する。copy fallback は VFS core が勝手に行わない。

### open file description

`Vfile` が offset を持つ。handle duplicate は同じ `Vfile` を参照するため offset を共有する。

### metadata consistency

`statx` は backend metadata generation と VFS cache generation を持てるようにする。最初は強い cache invalidation を実装しなくてもよいが、API は後で拡張できる形にする。

## Cache 方針

最初は `filed` 側に block cache / small page cache を置く。

理由。

- ext4 と btrfs の両方で利用できる
- Linux `.ko` 側の page cache assumption を完全再現しなくても始められる
- pkey data plane と組み合わせやすい

初期方針。

- cache は `filed` が所有する
- block I/O は `koboxd` block endpoint に出す
- dirty block writeback は backend/fsync policy に従う
- file data cache は `pread/pwrite` の上に後から乗せる
- mmap/pager-backed file mapping は初期 VFS では対象外

## Multi-thread 方針

`filed` は最初から multi-thread 前提の構造にする。

ただし初期実装で無理に高並列化しない。最初は single-thread 実行でもよいが、`Vnode`, `Vmount`, `Vfile`, `VfsHandle` の lifetime と lock を先に入れておき、後から worker pool を有効化しても意味論が変わらない形にする。

基本構造。

```text
filed
  main thread:
    IPC accept / service table / event dispatch

  worker threads:
    openat / pread / pwrite / statx / getdents

  global locks:
    mount_table_lock
    vnode_cache_lock
    client/session handle_table_lock

  object locks:
    Vmount.rwlock
    Vnode.rwlock
    Vfile.mutex
```

### Lock と lifetime

`Vnode` は refcount で lifetime を管理する。

unlink 済み vnode でも open file が残っていれば `Vfile` から参照され続ける。directory entry が消えることと vnode object が破棄されることを分離する。

`Vfile` は open file description であり、file offset を持つ。`read`, `write`, `lseek` は `Vfile.mutex` で offset を守る。`pread`, `pwrite` は offset を更新しないため、`Vfile.mutex` の保持時間を短くできる。

`Vmount` は mounted filesystem lifetime と root vnode を持つ。mount / unmount は `mount_table_lock` の exclusive lock を取る。path walk は mount table shared lock で走る。

### Path walk locking

path walk は parent vnode の shared lock を取りながら single component lookup を進める。

rename / unlink / mkdir / rmdir は対象 parent directory vnode の exclusive lock を取る。別 directory 間 rename では deadlock を避けるため、vnode identity の順序で parent locks を取る。

初期実装では RCU 的な lock-free path walk は使わない。まずは `rwlock + refcount + ordered locking` を正とする。

### Read/write 並列性

`pread` は file offset に触らないため、同じ `Vfile` に対して複数 worker が並列実行してよい。

`read` は offset を更新するため、offset の読み取りと更新は `Vfile.mutex` で直列化する。ただし実データ転送中に長時間 mutex を保持しない設計にする。

`pwrite` / `write` は vnode の write-side state、dirty cache、backend write ordering と関係する。write path を入れる前に dirty block policy と fsync policy を固定する。

### Directory cursor

`getdents` は directory cursor を handle として持つ。

cursor offset/state は `Vfile` または dedicated directory cursor object の mutex で守る。複数 client が同じ directory を open しても cursor は共有しない。handle duplicate した場合のみ cursor state を共有する。

### Worker pool

`filed` の main thread は IPC accept / dispatch に集中する。

重い operation は worker thread に渡す。

- path walk を伴う `openat`
- block I/O を伴う `pread` / `pwrite`
- directory scan を伴う `getdents`
- metadata I/O を伴う `statx`
- `fsync`

`filed <-> koboxd` の block I/O は async request として扱う。初期実装で synchronous call に見えても、内部 API は worker が待てる形にする。

### pkey ring と thread

pkey data plane は per-client/per-service ring を基本にする。

複数 client が同じ ring を共有しない。これにより ring lock と rights 管理を単純に保つ。

trusted client の `client <-> filed` fast path は pkey ring を使えるが、VFS object lifetime と rights は常に `filed` 側の handle table が所有する。shared memory 上に vnode pointer, Vfile pointer, backend pointer を置かない。

### 実装順

multi-thread 対応の実装順。

1. single-thread 実行でも `Vnode`, `Vmount`, `Vfile`, `VfsHandle` に refcount / lock field を入れる
2. handle table と vnode cache の lock を入れる
3. `pread`, `statx`, `getdents` を worker に投げられる internal API にする
4. `openat` path walk に shared/exclusive lock policy を入れる
5. `pwrite`, `fsync`, `rename`, `unlink` を入れる前に write lock policy を固定する
6. worker pool を有効化する
7. pkey data plane を per-client ring として追加する

非目標。

- 初期実装で lock-free vnode cache を作ること
- 初期実装で RCU path walk を作ること
- shared memory ring に VFS internal pointer を流すこと
- correctness より並列性能を優先すること

## libvfs と musl/POSIX layer

PachaOS は 2 つのユーザー向け層を提供する。

### libvfs

`libvfs` は PachaOS native daemon 向けの API である。

特徴。

- vnode / mount / file handle を opaque handle として扱う
- rights を明示する
- normal IPC と pkey fast IPC backend を切り替えられる
- `openat` / `pread` / `pwrite` / `statx` / `getdents` を直接提供する
- POSIX の曖昧さを無理に再現しない

### musl/POSIX layer

musl 側は POSIX API を `libvfs` に接続する。

担当するもの。

- `open`, `openat`
- `read`, `write`, `pread`, `pwrite`
- `close`
- `lseek`
- `stat`, `fstat`, `fstatat`, `statx`
- `opendir`, `readdir`, `closedir`
- `fsync`, `fdatasync`
- errno conversion
- process-local fd table integration
- stdio integration

POSIX compatibility は musl layer で吸収する。`libvfs` を POSIX API そのものにしない。

構造。

```text
app / daemon
  |
  | POSIX API: fopen/open/read/readdir
  v
musl libc
  |
  | PachaOS file backend
  v
libvfs
  |
  | normal IPC control plane / pkey data plane
  v
filed VFS
  |
  | vnode backend ops
  v
ext4 backend now / btrfs backend later
  |
  | block service IPC
  v
koboxd NVMe block provider
```

## IPC

VFS では 2 種類の IPC 経路を扱う。

### client <-> filed

PachaOS native daemon と musl/POSIX layer は `filed` に接続する。

control plane は normal IPC / fd passing を使う。

対象。

- service discovery
- root handle acquisition
- `openat`
- handle dup/close/transfer
- rights attenuation
- namespace control
- pkey ring setup
- fallback operation

data plane は trusted 専用の pkey shared-memory backend を使えるようにする。

対象。

- file read/write payload
- directory entries
- stat result batch
- larger request/response buffer

方針。

- pkey backend は optional
- normal IPC fallback を常に維持する
- fd passing は pkey ring に載せない
- pkey は trusted 別 process の高速 IPC として扱い、untrusted security boundary とはしない
- ring は per-client/per-service を基本にする
- client に server pointer は見せない

`filed` と seed/init/native daemon は trusted domain として pkey backend を使える。untrusted application や unknown process は normal IPC から始める。

### filed <-> koboxd

`filed` は block device endpoint を `koboxd` から受け取る。

最初は normal IPC の block read/write でよい。rootfs mount と VFS smoke を優先する。

後で `filed <-> koboxd` にも pkey data plane を使える。ただし最初に高速化する対象は `client <-> filed` の file payload である。

`koboxd` が公開する block API 候補。

- `block_get_info(device) -> sector_size, sector_count`
- `block_read(device, lba, sector_count, out_buffer)`
- `block_write(device, lba, sector_count, in_buffer)`
- `block_flush(device)`

## Rights

VFS handle は per-handle rights を持つ。

候補。

- `READ`
- `WRITE`
- `EXEC`
- `LOOKUP`
- `CREATE`
- `REMOVE`
- `RENAME`
- `STAT`
- `FSYNC`
- `DUP`
- `TRANSFER`
- `CLOSE`
- `ADMIN`

rights attenuation は `libvfs` / `filed` control plane で行う。kernel fd rights と同じ思想だが、file semantics は `filed` 内に閉じる。

## Error model

内部 error は PachaOS native status として扱う。musl/POSIX layer は errno に変換する。

例。

| VFS status | POSIX errno |
| --- | --- |
| `VFS_OK` | 0 |
| `VFS_NOT_FOUND` | `ENOENT` |
| `VFS_NOT_DIR` | `ENOTDIR` |
| `VFS_IS_DIR` | `EISDIR` |
| `VFS_EXISTS` | `EEXIST` |
| `VFS_DENIED` | `EACCES` / `EPERM` |
| `VFS_INVALID` | `EINVAL` |
| `VFS_CROSS_MOUNT` | `EXDEV` |
| `VFS_NOT_EMPTY` | `ENOTEMPTY` |
| `VFS_IO` | `EIO` |
| `VFS_UNSUPPORTED` | `ENOTSUP` |

## 実装順

### Step 1: koboxd block provider

- `koboxd` の NVMe sector0 read を block service API に整理する
- block device registry を作る
- block endpoint を公開する
- `storage_boot` の ext4 probe/read path に依存しない形にする

### Step 2: filed skeleton

- `filed` C binary を rootfs に置く
- `seed0root` が `filed` を起動する
- `filed` に VFS core directory を作る
- `Vnode`, `Vmount`, `Vfile`, `VfsHandle` を定義する
- root mount registry と handle table を作る

### Step 3: filed receives block endpoint

- `seed0root` または service table 経由で `filed` に `koboxd` block endpoint を渡す
- `filed` から block sector read smoke を通す

### Step 4: ext4 backend mount in filed

- `crc16.ko`, `mbcache.ko`, `jbd2.ko`, `ext4.ko` を `filed` 側で load/init
- ext4 rootfs probe を `filed` に移す
- root vnode を作る

### Step 5: vnode lookup/read

- component lookup
- regular file open
- `pread`
- `statx`
- root directory `getdents`

### Step 6: libvfs normal IPC

- `libvfs` を追加する
- normal IPC backend で `openat/pread/close/statx/getdents` を通す
- `seed0root` から `/sbin/koboxd.elf` または `/etc/pacha-release` を read する smoke を作る

### Step 7: pkey data plane

- trusted client 用 pkey ring setup
- normal IPC control message から ring を確立
- `pread` payload を pkey data plane に移す
- fallback と同じ意味論を保つ

### Step 8: write path

- `pwrite`
- file offset update
- dirty block handling
- `fsync`
- basic write smoke

### Step 9: POSIX/musl integration

- musl file backend を `libvfs` に接続
- `open/read/write/close/lseek/stat/readdir/fsync` を通す
- stdio smoke を rootfs file に対して行う

## 最初の smoke

最初の成功条件。

```text
[koboxd] block service ready
[filed] block read sector0 OK
[filed] vfs root ext4 mounted
[seed0root] vfs open /sbin/koboxd.elf OK
[seed0root] vfs read /sbin/koboxd.elf OK
```

次の成功条件。

```text
[seed0root] vfs statx /sbin/koboxd.elf OK
[seed0root] vfs getdents /sbin OK
[seed0root] vfs pwrite /tmp/vfs-smoke.txt OK
[seed0root] vfs pread /tmp/vfs-smoke.txt OK
```

## 非目標

初期 VFS でやらないこと。

- kernel VFS
- kernel file object
- `koboxd` に VFS を詰め込むこと
- POSIX API を `libvfs` に直実装すること
- untrusted process 向け pkey security boundary
- network filesystem
- mmap-backed file pager
- overlayfs
- FUSE compatibility
- Linux VFS の完全再実装

## 決定事項

- VFS は独立 daemon `filed` に置く
- `koboxd` は Linux driver `.ko` runtime / NVMe block provider に集中する
- core abstraction は vnode model とする
- ext4 を first backend、btrfs を future backend とする
- Linux FS `.ko` adapter は VFS core から分離する
- PachaOS native API は `libvfs`
- POSIX API は musl layer
- `client <-> filed` IPC は normal IPC control plane と pkey shared-memory data plane に分ける
- `filed <-> koboxd` は block service IPC とし、最初は normal IPC でよい
- pkey backend は trusted 専用であり、untrusted isolation として扱わない
