# filed VFS Design

## 目的

`koboxd` まで NVMe と Linux `.ko` loader/backend が到達したので、次の主対象は rootfs の顔になる VFS である。

ただし VFS を `koboxd` に入れない。`koboxd` は Linux `.ko` runtime / device backend / block provider / filesystem backend provider に集中させる。VFS は独立 daemon の `filed` が担当する。

VFS は単なる `open/read` smoke ではなく、PachaOS の userland が日常的に触るファイル API の基盤になる。そのため、既存 Unix / BSD の堅牢なファイル意味論と、Linux ext4 / 将来の btrfs backend が要求する引数・状態・呼び出し順の両方を満たす必要がある。

この文書では、`filed` VFS、PachaOS native `libvfs`、musl/POSIX layer、trusted pkey IPC backend、`koboxd` block/filesystem backend provider との境界を固定する。

## Daemon 分離

恒久構造は次のようにする。

```text
storage_boot
  -> seed0root
    -> koboxd  : .ko loader / capsule backend / NVMe block + ext4/btrfs backend daemon
    -> filed   : VFS / vnode / mount / file handle server
```

`koboxd` の責務。

- capsule device fd を受け取る
- PCI config / BAR / DMA / IRQ を `libcapsule` 経由で扱う
- NVMe backend を作る
- `nvme-auth.ko`, `nvme-core.ko`, `nvme.ko` を load/init する
- `crc16.ko`, `mbcache.ko`, `jbd2.ko`, `ext4.ko` を load/init する
- 将来 `btrfs.ko` と依存 `.ko` を load/init する
- block device service endpoint を公開する
- filesystem backend endpoint を公開する
- Linux driver / filesystem `.ko` runtime に集中する

`filed` の責務。

- rootfs filesystem backend endpoint を `koboxd` から受け取る
- ext4 / btrfs backend endpoint を mount する
- vnode / mount / open file description / handle table / path walk を持つ
- `libvfs` / musl から呼ばれる file service endpoint を公開する
- VFS cache と namespace policy を所有する

この分離により、`koboxd` が VFS server まで兼任して肥大化することと、Linux `.ko` runtime が複数 daemon に散らばることの両方を避ける。将来 `netd`, `inputd`, `displayd` などを増やす場合も、`koboxd` は Linux `.ko` backend runtime、`filed` は file namespace server という役割が保たれる。

## 基本方針

- kernel に VFS / FS / block file semantics を入れない
- `koboxd` は block provider と filesystem backend provider とする
- `filed` を VFS server とし、namespace と file handle lifetime を所有させる
- VFS core は PachaOS の vnode model として設計する
- ext4 / btrfs は `koboxd` の filesystem backend endpoint として扱い、Linux の `inode` / `dentry` / `file` 構造体を `filed` core に漏らさない
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
- backend object id
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
- filesystem backend endpoint
- mount flags
- backend mount handle

cross-mount operation は VFS core が判断する。`rename` の cross-mount は `EXDEV` 相当を返す。

### Vfile

`Vfile` は open file description を表す。

保持するもの。

- vnode pointer
- current offset
- open flags
- rights
- backend file handle
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

ただし PachaOS VFS core は Linux VFS の clone にはしない。Linux `.ko` が要求する `struct inode`, `struct dentry`, `struct file`, `address_space`, `kiocb`, `iov_iter` などは `koboxd` の backend adapter が作る。

`filed` は Linux object を直接管理しない。`filed` は `FsBackendOps` endpoint を mount し、そこから返される opaque backend object id を `Vnode` identity に使う。

責務分離。

| layer | 責務 |
| --- | --- |
| VFS core | vnode, mount, path walk, handle lifetime, rights, common semantics |
| filed FS backend client | VnodeOps を `koboxd` FS backend endpoint に変換 |
| koboxd FS backend adapter | FS backend request を Linux `.ko` 呼び出しに変換 |
| ext4/btrfs `.ko` | filesystem implementation inside `koboxd` |
| koboxd block provider | NVMe block service |

`koboxd` Linux adapter に閉じ込めるもの。

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

`filed` と `koboxd` の FS backend endpoint は、初期実装から request id / timeout / cancellation / data plane slot を持つ final-shape operation とする。

- `fs_mount_root(block_device, fs_type) -> fs_backend`
- `fs_lookup(parent_backend_id, name) -> child_backend_id, metadata`
- `fs_open(backend_id, flags) -> backend_file`
- `fs_pread(backend_file, offset, length) -> bytes`
- `fs_pwrite(backend_file, offset, bytes) -> bytes`
- `fs_statx(backend_id or backend_file) -> metadata`
- `fs_getdents(backend_dir, cursor) -> entries`
- `fs_fsync(backend_file, flags)`
- `fs_unlink(parent_backend_id, name)`
- `fs_rename(old_parent, old_name, new_parent, new_name, flags)`

`filed` はこの endpoint を VnodeOps として使う。path walk、rights、open file description、mount namespace は `filed` が持つ。

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

最初は `filed` 側に VFS cache / small file data cache を置く。

理由。

- ext4 と btrfs の両方で利用できる
- Linux `.ko` 側の page cache assumption を完全再現しなくても始められる
- pkey data plane と組み合わせやすい

初期方針。

- cache は `filed` が所有する
- filesystem I/O は `koboxd` FS backend endpoint に出す
- dirty file data writeback は backend/fsync policy に従う
- file data cache は `pread/pwrite` の上に後から乗せる
- mmap/pager-backed file mapping は初期 VFS では対象外

## Multi-thread 方針

`filed` は最初から multi-thread 前提の構造にする。

ただし初期実装で無理に高並列化しない。bring-up 時点でも main thread 直呼びではなく worker queue を通す。worker 数は 1 から始められるが、`Vnode`, `Vmount`, `Vfile`, `VfsHandle` の lifetime と lock、request id、timeout、cancellation の意味論は最初から有効にする。

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

ただし RCU / lock-free は恒久的な非目標ではなく、後期実装として必ず入れる。VFS の意味論と lock order が固まり、race test と stress test が整った後で、path walk と vnode cache の hot path を RCU / lock-free に移行する。

### Read/write 並列性

`pread` は file offset に触らないため、同じ `Vfile` に対して複数 worker が並列実行してよい。

`read` は offset を更新するため、offset の読み取りと更新は `Vfile.mutex` で直列化する。ただし実データ転送中に長時間 mutex を保持しない設計にする。

`pwrite` / `write` は vnode の write-side state、dirty cache、backend write ordering と関係する。write path を入れる前に dirty file data policy と fsync policy を固定する。

### Directory cursor

`getdents` は directory cursor を handle として持つ。

cursor offset/state は `Vfile` または dedicated directory cursor object の mutex で守る。複数 client が同じ directory を open しても cursor は共有しない。handle duplicate した場合のみ cursor state を共有する。

### Worker pool

`filed` の main thread は IPC accept / dispatch に集中する。

重い operation は worker thread に渡す。

- path walk を伴う `openat`
- filesystem backend I/O を伴う `pread` / `pwrite`
- directory scan を伴う `getdents`
- metadata I/O を伴う `statx`
- `fsync`

`filed <-> koboxd` の FS backend I/O は async request として扱う。初期実装で synchronous call に見えても、内部 API は worker が待てる形にする。

### pkey ring と thread

pkey data plane は per-client/per-service ring を基本にする。

複数 client が同じ ring を共有しない。これにより ring lock と rights 管理を単純に保つ。

trusted client の `client <-> filed` fast path は pkey ring を使えるが、VFS object lifetime と rights は常に `filed` 側の handle table が所有する。shared memory 上に vnode pointer, Vfile pointer, backend pointer を置かない。

### 実装順

multi-thread 対応の実装順。

1. worker queue 経由の実行を前提に `Vnode`, `Vmount`, `Vfile`, `VfsHandle` に refcount / lock field を入れる
2. handle table と vnode cache の lock を入れる
3. `pread`, `statx`, `getdents` を worker に投げられる internal API にする
4. `openat` path walk に shared/exclusive lock policy を入れる
5. `pwrite`, `fsync`, `rename`, `unlink` を入れる前に write lock policy を固定する
6. worker pool を有効化する
7. pkey data plane を per-client ring として追加する
8. race / stress test を増やす
9. path walk read-side を RCU 化する
10. vnode cache lookup を lock-free / mostly lock-free 化する
11. metadata snapshot に seqlock を導入する

非目標。

- shared memory ring に VFS internal pointer を流すこと
- correctness より並列性能を優先すること

後期実装として入れるもの。

- RCU read-side path walk
- lock-free / mostly lock-free vnode cache lookup
- seqlock による metadata snapshot
- per-cpu / per-worker small cache
- directory entry negative cache

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
  | FsBackendOps endpoint
  v
koboxd ext4 backend now / btrfs backend later
  |
  | in-process block provider call
  v
koboxd NVMe block provider
```

## IPC

VFS 関連 IPC は初期実装から final-shape protocol とする。

temporary shortcut は作らない。bring-up smoke でも versioned header、request id、timeout、cancellation、endpoint worker queue、control/data plane 分離を通す。

normal IPC は fallback であり、設計の後回しではない。pkey data plane と normal IPC fallback は同一の request model / status model / lifetime model を共有する。

### koboxd IPC substrate

`koboxd` は Linux `.ko` backend を endpoint 化して公開する daemon である。

daemon は 1 つだが、service endpoint は分ける。

```text
koboxd
  control endpoint
    - service discovery
    - runtime/module/device/fs-backend registry
    - endpoint creation
    - handle close/dup/attenuate
    - cancellation
    - metrics/debug dump

  block endpoint
    - block_get_info
    - block_read
    - block_write
    - block_flush

  fs-backend endpoint
    - fs_mount_root
    - fs_lookup
    - fs_open
    - fs_close
    - fs_pread
    - fs_pwrite
    - fs_getdents
    - fs_fsync
    - fs_statx
    - fs_rename/unlink/mkdir/rmdir

  event endpoint
    - hotplug
    - media change
    - backend error
    - async completion
```

`koboxd` IPC の必須要件。

- versioned protocol header
- request id
- timeout
- cancellation
- per-endpoint worker queue
- per-endpoint metrics/debug dump
- object handle / rights / lifetime
- control plane / data plane 分離
- pkey shared-memory data plane
- normal IPC fallback with identical semantics

### Message header

control message は固定 header を持つ。

```c
struct kobox_msg {
    uint32_t version;
    uint32_t opcode;
    uint64_t request_id;
    uint64_t object_id;
    uint64_t rights;
    uint64_t flags;
    uint64_t timeout_ns;
    uint64_t data_slot;
    uint64_t args[6];
};
```

reply も固定 header を持つ。

```c
struct kobox_reply {
    uint32_t version;
    int32_t status;
    uint64_t request_id;
    uint64_t result;
    uint64_t bytes;
    uint64_t out_handle;
    uint64_t flags;
};
```

large payload は header に直接載せない。

```text
control msg:
  opcode
  request_id
  object handle
  offset / length
  data_slot

data plane:
  pkey/shared ring slot

completion:
  request_id
  status
  bytes
```

### Control plane

control plane は normal IPC / fd passing を使う。

対象。

- service discovery
- endpoint creation
- pkey ring setup
- object handle creation
- object handle close/dup/attenuate
- fd passing
- cancellation
- mount/open/close/fsync などの lifetime operation
- metrics/debug dump

fd passing は pkey ring に載せない。rights attenuation は control plane の責務である。

### Data plane

trusted domain の data plane は pkey shared-memory ring を使う。

対象。

- `block_read`
- `block_write`
- `fs_pread`
- `fs_pwrite`
- `fs_getdents`
- large `statx` / xattr / future metadata batch

ring は per-client/per-service を基本にする。複数 client が同じ ring を共有しない。

pkey は trusted 別 process の高速 IPC として扱い、untrusted security boundary とはしない。untrusted process は normal IPC fallback に落とすが、request/reply/status/lifetime の意味論は変えない。

### Cancellation and timeout

すべての request は `request_id` を持つ。

timeout は protocol field として持ち、server 側 worker queue でも見えるようにする。

cancellation は control endpoint に送る。

```text
cancel(request_id)
```

cancellation は best-effort でよいが、完了済み request と racing しても request id で結果を一意に扱える必要がある。

### Worker queues

`koboxd` は endpoint ごとに worker queue を分ける。

- control worker queue
- block worker queue
- fs-backend worker queue
- event worker queue

module/runtime lock、device registry lock、block registry lock、fs backend registry lock、request queue lock は分ける。1 giant lock は禁止する。

### client <-> filed

PachaOS native daemon と musl/POSIX layer は `filed` に接続する。

`filed` IPC も `koboxd` と同じ思想で、初期実装から control/data plane を分ける。

control plane 対象。

- service discovery
- root handle acquisition
- `openat`
- handle dup/close/transfer
- rights attenuation
- namespace control
- pkey ring setup
- cancellation

data plane 対象。

- file read/write payload
- directory entries
- stat result batch
- larger request/response buffer

`filed` と seed/init/native daemon は trusted domain として pkey backend を使う。untrusted application や unknown process は normal IPC fallback を使う。

### filed <-> koboxd

`filed` は filesystem backend endpoint を `koboxd` から受け取る。

`koboxd` は block device endpoint も持つが、`filed` が直接 block device を読む経路は debug / bring-up 用に限定する。通常経路では、`filed` は `koboxd` の ext4/btrfs backend endpoint を VnodeOps として使う。

`filed <-> koboxd` も初期実装から control/data plane を分ける。

control plane 対象。

- `fs_mount_root`
- `fs_lookup`
- `fs_open`
- `fs_close_backend_file`
- `fs_fsync`
- `fs_unlink`
- `fs_rename`
- backend handle close/dup/attenuate
- cancellation

data plane 対象。

- `fs_pread`
- `fs_pwrite`
- `fs_getdents`
- future xattr / metadata batch

`koboxd` が公開する block API。

- `block_get_info(device) -> sector_size, sector_count`
- `block_read(device, lba, sector_count, out_buffer)`
- `block_write(device, lba, sector_count, in_buffer)`
- `block_flush(device)`

`koboxd` が公開する filesystem backend API。

- `fs_mount_root(block_device, fs_type) -> fs_backend`
- `fs_lookup(fs_backend, parent_id, name) -> child_id, metadata`
- `fs_open(fs_backend, object_id, flags) -> backend_file`
- `fs_close_backend_file(backend_file)`
- `fs_pread(backend_file, offset, length, out_buffer)`
- `fs_pwrite(backend_file, offset, in_buffer)`
- `fs_statx(fs_backend, object_id or backend_file)`
- `fs_getdents(fs_backend, dir_id, cursor, out_buffer)`
- `fs_fsync(backend_file, flags)`
- `fs_unlink(fs_backend, parent_id, name)`
- `fs_rename(fs_backend, old_parent, old_name, new_parent, new_name, flags)`

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

### Step 1: koboxd IPC substrate

- control / block / fs-backend / event endpoint を定義する
- versioned protocol header と reply header を実装する
- request id / timeout / cancellation を入れる
- per-endpoint worker queue を作る
- pkey shared-memory data plane を作る
- normal IPC fallback を同一意味論で実装する
- per-endpoint metrics/debug dump を入れる

### Step 2: koboxd block provider

- `koboxd` の NVMe sector0 read を block service API に整理する
- block device registry を作る
- block endpoint を公開する
- `block_read` / `block_write` は data plane を通す
- cancellation / timeout / metrics を通す
- `storage_boot` の ext4 probe/read path に依存しない形にする

### Step 3: koboxd filesystem backend provider

- `crc16.ko`, `mbcache.ko`, `jbd2.ko`, `ext4.ko` を `koboxd` 側で load/init する
- ext4 rootfs probe を `koboxd` の FS backend provider に移す
- `fs_mount_root`, `fs_lookup`, `fs_open`, `fs_pread`, `fs_statx`, `fs_getdents` を backend endpoint として整理する
- `fs_pread` / `fs_pwrite` / `fs_getdents` は data plane を通す
- `fs_lookup` / `fs_open` / `fs_fsync` は control plane を通す
- cancellation / timeout / metrics を通す
- `koboxd` 内で Linux `.ko` runtime と Linux FS adapter を完結させる

### Step 4: filed skeleton

- `filed` C binary を rootfs に置く
- `seed0root` が `filed` を起動する
- `filed` に VFS core directory を作る
- `Vnode`, `Vmount`, `Vfile`, `VfsHandle` を定義する
- root mount registry と handle table を作る
- `filed` 側も request id / timeout / cancellation / worker queue を持つ

### Step 5: filed receives FS backend endpoint

- `seed0root` または service table 経由で `filed` に `koboxd` FS backend endpoint を渡す
- debug 用に block endpoint も受け取れるようにする
- `filed` から `fs_mount_root` smoke を通す

### Step 6: ext4 backend mount through koboxd

- `filed` が `koboxd` FS backend endpoint 経由で ext4 rootfs を mount する
- `filed` は Linux inode/dentry/file を持たない
- `koboxd` が返す root backend object id から root vnode を作る

### Step 7: vnode lookup/read

- component lookup
- regular file open
- `pread`
- `statx`
- root directory `getdents`
- `pread` / `getdents` は `client <-> filed` data plane と `filed <-> koboxd` data plane を通す

### Step 8: libvfs IPC backend

- `libvfs` を追加する
- control plane / data plane 両対応で `openat/pread/close/statx/getdents` を通す
- normal IPC fallback を同一意味論で通す
- `seed0root` から `/sbin/koboxd.elf` または `/etc/pacha-release` を read する smoke を作る

### Step 9: client <-> filed pkey data plane

- trusted client 用 pkey ring setup
- normal IPC control message から ring を確立
- `pread` payload を pkey data plane に移す
- fallback と同じ意味論を保つ

### Step 10: write path

- `pwrite`
- file offset update
- dirty file data handling
- `filed` cache と `koboxd` FS backend writeback policy の接続
- `fsync`
- basic write smoke

### Step 11: POSIX/musl integration

- musl file backend を `libvfs` に接続
- `open/read/write/close/lseek/stat/readdir/fsync` を通す
- stdio smoke を rootfs file に対して行う

### Step 12: filed <-> koboxd pkey data plane

- `fs_pread` / `fs_pwrite` / `fs_getdents` payload を pkey data plane に移す
- `filed` VFS semantics と `koboxd` Linux FS backend semantics を変えずに高速化する
- normal IPC fallback を維持する

### Step 13: RCU path walk

- race / stress test を増やす
- path walk read-side を RCU 化する
- rename / unlink / mount crossing との整合性を検証する
- fallback lock path を残しながら段階的に hot path を移行する

### Step 14: lock-free vnode cache

- vnode cache lookup を lock-free / mostly lock-free 化する
- cache miss / insert / eviction は既存 lock path で保守的に扱う
- negative dentry / negative lookup cache を入れる
- metadata snapshot に seqlock を導入する

### Step 15: per-worker cache

- per-worker small cache を入れる
- hot directory / vnode lookup の局所性を使う
- pkey data plane と組み合わせた read hot path を測る
- correctness を race / stress test で維持する

## 最初の smoke

最初の成功条件。

```text
[seed0root] koboxd control discover OK
[seed0root] koboxd pkey ring setup OK
[koboxd] block service ready
[koboxd] fs backend ext4 ready
[filed] koboxd fs backend endpoint ready
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
- `koboxd` は Linux `.ko` runtime / NVMe block provider / ext4-btrfs FS backend provider に集中する
- core abstraction は vnode model とする
- ext4 を first backend、btrfs を future backend とする
- Linux FS `.ko` adapter は `filed` VFS core から分離し、`koboxd` に集約する
- PachaOS native API は `libvfs`
- POSIX API は musl layer
- `koboxd` IPC は初期実装から final-shape protocol とする
- `koboxd` IPC は request id / timeout / cancellation / metrics を必須にする
- `koboxd` は control / block / fs-backend / event endpoint を分ける
- `koboxd` は endpoint ごとの worker queue を持つ
- `client <-> filed` IPC は normal IPC control plane と pkey shared-memory data plane に分ける
- `filed <-> koboxd` は FS backend service IPC とし、初期実装から pkey data plane と normal IPC fallback を同一意味論で持つ
- pkey backend は trusted 専用であり、untrusted isolation として扱わない
