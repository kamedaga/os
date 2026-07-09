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
- pathname based process exec service を公開する
- executable file read, ELF validation, process image construction, initial stack / auxv construction を行う
- VFS cache と namespace policy を所有する

この分離により、`koboxd` が VFS server まで兼任して肥大化することと、Linux `.ko` runtime が複数 daemon に散らばることの両方を避ける。将来 `netd`, `inputd`, `displayd` などを増やす場合も、`koboxd` は Linux `.ko` backend runtime、`filed` は file namespace server という役割が保たれる。

### bootstrap exec exception

`seed0root` には bootstrap 用の最小 exec 実装を残す。

理由は起動順にある。

```text
seed0boot
  -> storage_boot
    -> seed0root
      -> koboxd
      -> filed
```

`filed` が起動する前に、`seed0root` は少なくとも `koboxd` と `filed` 自身を起動しなければならない。そのため、`seed0root` の ELF load / process start は完全には消せない。

ただしこれは通常の process exec authority ではない。`seed0root` の exec は bootstrap executor としての例外であり、通常の `/sbin/...`、service spawn、user process exec は `filed` に集約する。

恒久的な責務分離。

```text
seed0root:
  - bootstrap fd を読む
  - koboxd を起動する
  - filed を起動する
  - filed に fs backend endpoint / control endpoint を渡す

filed:
  - namespace / VFS handle / vnode / mount を管理する
  - path lookup と file read を行う
  - exec by path を提供する
  - argv/env/auxv/bootstrap fd/inherit fd policy を決める
  - process/thread fd を返す
```

## 基本方針

- kernel に VFS / FS / block file semantics を入れない
- `koboxd` は block provider と filesystem backend provider とする
- `filed` を VFS server とし、namespace と file handle lifetime を所有させる
- VFS core は PachaOS の vnode model として設計する
- ext4 / btrfs は `koboxd` の filesystem backend endpoint として扱い、Linux の `inode` / `dentry` / `file` 構造体を `filed` core に漏らさない
- PachaOS native daemon 向けに `libvfs` を提供する
- POSIX 互換 API は musl layer で提供し、`libvfs` を POSIX API そのものにしない
- `filed` との高速 IPC は trusted 専用の pkey shared-memory backend を使えるようにする
- normal IPC / fd passing は control plane としてだけ維持する
- `filed` の VFS core と exec core は Coq 抽象モデルを先に作り、C 実装はそのモデルに寄せる
- VST は初期対象にしない。まず Coq の executable model、invariant preservation、同一ケースの C differential test で設計の有効性を見る

## Coq 抽象モデル方針

`filed` は scheduler と同じく、バグると OS 全体の足場が崩れる領域である。特に VFS と exec は、path walk、handle lifetime、rights、fd inheritance、ELF segment validation、process construction が絡むため、C を直接育てると状態遷移の見落としが起きやすい。

そのため、VST までは行かずに、まず Coq の抽象モデルを設計の中心に置く。

目的。

- C 実装前に VFS / exec の状態遷移を固定する
- C が壊してはいけない invariant を先に列挙する
- `filed` の dirty な helper 群を、モデル由来の小さい関数へ自然に分解する
- 仕様とテストケースを同じ形で Coq / C に持てるようにする
- kernel に逃げず、userland service として正しく解く
- SMP では複数 core/client からの request を filed 境界で sequence 付きに直列化し、pure VFS / exec state への適用順を明示する

非目的。

- 初期段階で VST proof を作ること
- Linux VFS を Coq で完全再現すること
- ext4 / btrfs 内部構造を Coq model に入れること
- IPC data plane や pkey ring のメモリ安全性をこの model で証明すること
- kernel process object / fd table の正しさを filed model が証明すること

Coq model は `filed` の pure core を扱う。kernel syscall、IPC、pkey ring、VMO map、process/thread create は adapter の effect として外に出す。

```text
verified/filed/
  spec/
    FiledTypes.v
    VfsModel.v
    VfsSpec.v
    ExecModel.v
    ExecSpec.v
    FiledRuntimeSpec.v
    MulticoreModel.v
    MulticoreSpec.v
  tests/
    cases/
```

`verified/filed/spec` は Coq 抽象モデルを置く場所である。C 実装は `userland/filed` に集約する。`filed` は実装量が大きく、bootstrap、IPC、pkey ring、rootfs integration、daemon lifecycle と密接に絡むため、`verified/filed/c` のような別実装ツリーには分けない。

恒久方針。

- `verified/filed/spec`: 仕様、抽象モデル、補題
- `userland/filed`: C 実装、daemon glue、IPC、backend adapter
- `pacha_docs/filed-vfs-design.md`: 設計判断と段階計画

開発段階の Markdown は増やさない。`filed` / VFS / exec / verified filed に関する設計判断、段階計画、モデルと C の対応、検証コマンドはこの `pacha_docs/filed-vfs-design.md` に集約する。`userland/filed` や `verified/filed` 配下には、必要なソース、テスト、Coq ファイル、スクリプトだけを置く。

C 側は `verified/filed/spec` の関数分解に寄せる。たとえば path walk、open file description、handle table、rename commit、exec plan は model と同じ責務境界を持つ。ただし C source の実体は `userland/filed` に置く。

### SMP / multicore model

`filed` は userland daemon なので、複数 CPU が同時に VFS state を直接更新する構造にはしない。SMP で model 化する対象は、複数 core/client から届く request を filed core がどの順序で pure state transition に適用するかである。

抽象化するもの。

```text
FiledRequest:
  core_id
  client_id
  op

FiledRuntimeState:
  vfs_state
  next_sequence
  request_log
```

基本 invariant。

- `next_sequence` は monotonic
- request log の sequence は一意
- log entry の sequence は常に `next_sequence` より小さい
- 1 request は 1 pure VFS / exec operation として直列化される
- operation が `well_formed_state` を保存するなら、multicore runtime も `well_formed_state` を保存する

この model は lock-free ring、mutex、worker thread 数、pkey IPC layout を固定しない。C 実装ではそれらを変えられるが、filed core に入る直前で sequence を切り、`state + request -> state + response` の形に落とす。

### VFS model

VFS 抽象状態。

```text
VfsState:
  mounts
  vnodes
  handles
  cwd table later
  next ids
```

最小 entity。

```text
Mount:
  mount_id
  root_vnode
  backend_id
  fs_kind
  flags

Vnode:
  vnode_id
  mount_id
  backend_object_id
  kind
  parent
  name
  linked
  symlink_target
  generation
  refcount
  cached_stat

Vfile:
  file_id
  vnode_id
  offset
  status_flags
  rights
  refcount

VfsHandle:
  handle_id
  file_id | vnode_id | mount_id
  rights
  fd_flags
  generation
```

基本 invariant。

- mount id は一意
- active vnode id は一意
- `(mount_id, backend_object_id)` は active vnode 内で一意
- active handle id は一意
- active handle は存在する object を指す
- active file は存在する vnode を指す
- file offset は non-negative
- unlink / rmdir は vnode object を即座に消さず、directory lookup から外すために `linked=false` にする
- open file description が参照する vnode は unlink 後も state 内に残る
- `Vfile` は Unix の open file description であり、`offset` と file status flags を持つ
- `VfsHandle` は fd 相当であり、`CLOEXEC` など fd-local flags を持つ
- `dup` / attenuation は同じ `Vfile` を共有し、fd-local flags は新しい handle 側で独立する
- `CLOEXEC` handle は exec inheritance set に入らない
- directory-only operation は directory vnode にだけ成功する
- regular-file read/write は regular vnode にだけ成功する
- rights の attenuation は rights を増やさない
- closed handle は lookup できない
- stale generation の handle は使えない
- path walk は root / cwd / mount boundary / `.` / `..` を仕様通り処理する
- path walk は budget を持ち、symlink loop / excessive expansion を `LOOP` 相当で止める
- mount root での `..` はその mount root に留まり、親 mount へ暗黙に脱出しない
- final symlink は通常 follow するが、`O_NOFOLLOW` では follow せず `openat` 側で拒否する
- cross-mount rename は `VFS_CROSS_MOUNT`
- backend object id の内容は opaque で、VFS core は比較以外に解釈しない
- `O_CREAT` は parent path が存在し、caller に create right があり、final component が存在しない場合だけ regular vnode を作る
- `O_EXCL | O_CREAT` は final component が既に存在する場合 `EXIST` 相当で失敗する
- `O_DIRECTORY` は non-directory target を拒否する
- `O_NOFOLLOW` は final symlink target を拒否する
- `O_TRUNC` は write right と regular vnode を要求し、許可された場合は truncate backend decision を返す

最初に Coq 関数として定義する操作。

```text
mount_root
lookup_component
path_walk
openat
close
dup_attenuate
read_prepare
pread_prepare
pwrite_prepare
getdents_prepare
statx
rename
unlink
mkdir
rmdir
```

`path_walk_context` は root start / cwd start を明示する。相対 path は cwd から、絶対相当の path は root から開始する。symlink 展開は budget を消費し、loop / excessive expansion は `FiledErrLoop` に落とす。

`*_prepare` は backend I/O を直接行わない。VFS model は「この backend request を出してよい」という decision を返す。実際の `koboxd` request は C adapter が実行する。

`read_prepare` は Unix `read` 相当として `Vfile.offset` を使い、成功時に共有 open file description の offset を進める。`pread_prepare` は caller supplied offset を使い、`Vfile.offset` を更新しない。

現在の C 実装では `filed_open_file_t` がこの `Vfile` に対応する。

- `filed_open_file_t.vnode_id` は backend object ではなく filed 内部 vnode handle を指す
- `filed_open_file_t.offset` は `READ` / `GETDENTS` が共有する open file description offset
- `filed_open_file_t.status_flags` は `APPEND` / `NONBLOCK` / `SYNC` など file status flags
- `filed_open_file_t.rights` は open file description が backend I/O に使える権限
- `filed_handle_t.fd_flags` は `CLOEXEC` など fd-local flags

`OPENAT` は vnode handle ではなく、常に open file handle を返す。`READ` は `read_prepare` で現在 offset を backend request に変換し、backend が実際に返した byte 数だけ `read_commit` で offset を進める。`PREAD` は caller supplied offset を使い、commit を持たない。`GETDENTS` も directory open file description の offset を使い、返した entry 数だけ `getdents_commit` で進める。

`DUP` は `filed_vfs_dup_handle` で同じ `filed_open_file_t` を指す新しい `filed_handle_t` を作る。新 handle の `CLOEXEC` は caller が明示する。`GET_FLAGS` / `SET_FLAGS` は `CLOEXEC` を handle-local に、`APPEND` / `NONBLOCK` / `SYNC` を共有 open file description に反映する。これにより、`dup` 後に offset と file status flags は共有され、exec inheritance policy は fd ごとに独立する。

`PWRITE` / `WRITE` / `FSYNC` は `koboxd` の ext4 backend write path に接続する。`PWRITE` は caller supplied offset を使い、`filed_open_file_t.offset` を更新しない。`WRITE` は current offset を使い、backend が実際に書けた byte 数だけ offset を進める。`FSYNC` は write 権限のある regular file handle に対して backend `fsync` を発行する。

初期実装では `APPEND` 付き `WRITE` は未対応として明示的に拒否する。append write は EOF 決定と write ordering policy が必要なので、`statx` / backend size refresh / write lock policy を固めてから入れる。

### Exec model

exec は `filed` の責務に入れる。ただし model は process creation syscall そのものを証明しない。`filed` が正しい exec plan を作ることを扱う。

抽象状態。

```text
ExecRequest:
  path
  argv
  env
  argc/envc consistency
  cwd/root handle later
  bootstrap_fd optional
  inherit fd list
  flags

ElfImage:
  bytes
  ehdr
  phdrs

ExecPlan:
  process_rights
  thread_rights
  mappings
  stack_image
  entry
  initial_sp
  auxv
  argv_layout
  env_layout
  inherited_fds
  inherited_handles
```

ELF / process image invariant。

- ELF magic / class / endian / version / machine が一致する
- ELF type は `EXEC` または `DYN`
- program header table は image 範囲内
- `PT_LOAD` segment は image 範囲内
- `memsz >= filesz`
- load segment の virtual range は page-aligned mapping に収まる
- load segments は overflow しない
- load segments は互いに不正に overlap しない
- writable/executable permission は segment flags からだけ作る
- stack は page aligned VMO として作る
- initial stack は argc/argv/env/auxv を含む
- argc/envc は argv/env の list length と一致する
- auxv は pagesz / entry / phent / phnum を含む
- `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_RANDOM`, `AT_EXECFN` が一貫する
- optional bootstrap fd は `PACHA_AT_BOOTSTRAP_FD` としてだけ渡る
- inherit fd list は明示された fd だけを含む
- `CLOEXEC` 相当の fd は通常 exec plan に含まれない
- filed の VFS handle table から exec inherit plan を作る場合、`VfsFdCloseOnExec` handle は inherited handle set に入らない
- ASLR が有効な場合でも segment 間の相対配置は保持される

最初に Coq 関数として定義する操作。

```text
validate_elf_header
collect_load_segments
choose_load_bias
build_mapping_plan
build_stack_plan
build_fd_inherit_plan
build_handle_inherit_plan
build_exec_plan
```

`build_exec_plan_with_handles` は effect を実行しない。返すのは「どの VMO を作るか」「どこへ map するか」「どの fd / filed handle を inherit するか」「どの argv/env/auxv を stack に置くか」「どの entry/sp で thread を作るか」という plan である。C adapter はこの plan を順に実行し、失敗したら作成済み fd を閉じる。

`build_exec_plan` は handle table を持たない互換 wrapper とする。通常の filed exec path は `filed_exec_path_with_handles` を使い、現在の VFS handle table から `CLOEXEC` filtering 済み inherited handles を作る。

現在の `EXEC_PATH` ABI は kernel fd inheritance と filed open file handle inheritance を分けて扱う。

- kernel fd は IPC fd passing で child process に渡す
- filed open file handle は `inherit_handles[]` で指定し、`filed_vfs_dup_handle_for_exec` が child 用 filed handle id を作る
- duplicated filed handle は同じ `filed_open_file_t` を指すため offset / status flags を共有する
- duplicated handle の `CLOEXEC` は落とす
- source handle に `CLOEXEC` が付いている場合、filed handle inheritance は拒否する
- bootstrap payload には kernel fd id と filed handle id の両方を patch できる

### VFS と exec の接続

`exec_path` は VFS model と Exec model の合成として扱う。

```text
exec_path(state, request):
  path_walk(open executable)
  check execute/read rights
  read file image or create file-backed image source
  build_exec_plan
  return decision
```

最初の仕様。

- path が存在しないなら `VFS_NOT_FOUND`
- path が directory なら `VFS_IS_DIR`
- execute/read 権限がなければ `VFS_DENIED`
- ELF が不正なら `EXEC_BAD_FORMAT`
- process image plan が overflow するなら `EXEC_INVALID_IMAGE`
- 成功時は VFS state を壊さない
- 成功時の plan は Exec invariant を満たす
- backend read が必要な場合、VFS model は read request decision を返すだけで payload は adapter が読む

### C 実装への落とし方

VST をしない代わりに、C 実装は model に寄せた小さい pure-ish 関数へ分ける。

```text
filed_vfs_mount_root
filed_vfs_lookup_component
filed_vfs_path_walk
filed_vfs_openat
filed_vfs_close

filed_exec_validate_elf
filed_exec_collect_segments
filed_exec_build_mapping_plan
filed_exec_build_stack_plan
filed_exec_build_fd_plan
filed_exec_start_plan
```

C 側の制約。

- dynamic allocation は adapter 境界に寄せ、core は固定長 table で始める
- result code は明示する
- out parameter を使う
- integer overflow check を明示する
- pointer を model の identity として扱わない
- backend object は opaque id と generation で扱う
- side effect をする関数と plan を作る関数を分ける
- failure unwind を一箇所に集約する

### differential / property test

VST なしで効果を見るため、最初は同じケースを Coq と C の両方に持つ。

VFS cases。

- root lookup
- missing path
- non-directory component
- `.` / `..`
- cross-mount traversal
- handle close then use
- rights attenuation
- duplicate handle generation
- regular file `pread_prepare`
- directory `getdents_prepare`

Exec cases。

- valid static executable
- invalid magic
- unsupported machine
- truncated phdr table
- segment filesz out of range
- `memsz < filesz`
- segment address overflow
- segment overlap
- no `PT_LOAD`
- bootstrap fd present / absent
- fd inherit list with cloexec filtering
- ASLR load bias preserves relative layout

合格条件。

- Coq examples が通る
- C unit test が同じ期待結果を返す
- rootfs 上の `/sbin/filed.elf` や `/sbin/koboxd.elf` に対して `build_exec_plan` smoke が通る
- seed0root の bootstrap exec と filed の normal exec の差分が文書化されている

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
- file status flags (`APPEND`, `NONBLOCK`, `SYNC`)
- rights
- backend file handle
- refcount

`dup` 相当では `Vfile` を共有し、file offset と file status flags も共有する。`open` し直した場合は別の `Vfile` になる。

### VfsHandle

`VfsHandle` は `filed` IPC から見える opaque handle である。

保持するもの。

- handle id
- points to `Vfile`, directory cursor, mount, or control object
- per-handle rights
- fd-local flags (`CLOEXEC`)
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

Coq model では `rename_commit` を単一 transition として扱う。destination が存在する場合は、同じ transition 内で destination を `linked=false` にし、source の `(parent, name)` を target へ更新する。cross-mount は `FiledErrCrossMount`、non-empty directory overwrite は `FiledErrNotEmpty` で失敗する。

### unlink / rmdir lifetime

`unlink` は regular / symlink / device など directory 以外の linked entry を `linked=false` にする。`rmdir` は directory だけを対象にし、linked child が残っていれば `FiledErrNotEmpty` で失敗する。

directory lookup は `linked=true` の vnode だけを返す。open file description は vnode id を保持するため、unlink 後も open file は read/write/fsync/close 可能である。

### open file description

`Vfile` が offset を持つ。handle duplicate は同じ `Vfile` を参照するため offset を共有する。

fd-local な `CLOEXEC` は `VfsHandle` が持つ。`APPEND`, `NONBLOCK`, `SYNC` のような file status flags は `Vfile` が持つ。これにより `dup` 後の offset / status 共有と、fd ごとの exec inheritance policy を分離する。

`read` / `write` / `lseek` は `Vfile.offset` を更新する。`pread` / `pwrite` は明示 offset を使い、`Vfile.offset` を更新しない。

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

### filed session fast path

現状の `FILED_V2_OP_SESSION_OPEN` session は shared page を使って payload copy は減らしているが、request metadata と completion は still normal IPC である。

```text
client:
  shared page に payload を書く
  IPC_SEND request metadata
  IPC_RECV reply

filed:
  wait_many(session channel)
  IPC_RECV request metadata
  shared page を読む/書く
  IPC_SEND reply
```

この形は correctness は良いが、`stat`, cached `pread`, dirty cached `pwrite`, `getdents` のように filed 内で完結する操作でも、最低 2 syscall と scheduler wakeup を払う。operation-level 計測ではこの床が 50us から 80us 付近に出ている。

fast path は現行 session protocol を置き換える。互換性維持用の別 protocol は作らない。`CONNECT` 後の session operation は request metadata と completion も shared memory に置く ring-only protocol にする。

```text
session page:
  header
  request ring
  completion ring
  payload slots

control channel:
  connect
  fd passing
  rights attenuation
  cancellation
  close/session teardown
  ring doorbell
  metrics/debug
```

古い session call path は残さない。normal IPC request/reply は session 確立、fd passing、権限変更、debug/metrics、exec のような control operation に限定する。VFS data operation の標準経路は ring である。

shared memory に置いてよいものは value semantics の descriptor だけである。

- request id
- opcode
- handle id
- rights-independent flags
- offset / length
- payload slot index / payload length
- timeout
- completion status / result

shared memory に置いてはいけないもの。

- vnode pointer
- Vfile pointer
- backend pointer
- kernel fd
- authority を増やせる token
- parent/child cap 関係

#### Ring layout

ring は per-session SPSC を基本にする。1 process から複数 thread が libc を呼ぶ場合は、libc 側で session lock を持つか、thread-local session を作る。最初は global session lock でよい。

```text
filed_fast_header:
  magic
  version
  flags
  request_capacity
  completion_capacity
  payload_slot_count
  request_head
  request_tail
  completion_head
  completion_tail
  doorbell_seq
  completion_seq

filed_fast_request:
  request_id
  opcode
  flags
  handle
  aux_handle
  offset
  length
  payload_slot
  payload_length
  timeout_ns

filed_fast_completion:
  request_id
  status
  result
  bytes
  flags
```

request ring と completion ring は固定 slot 数にするが、batch 回数は固定しない。libc は ring が空いている限り enqueue し、同期 syscall の意味が必要なところで flush/wait する。つまり `readv` や stdio flush のような自然なまとまりでは複数 request を一回の doorbell にまとめられるが、単発 syscall は単発のままでも動く。

#### Doorbell and completion

初期実装は kernel ABI を変えない。

1. client が request ring に 1 個以上 enqueue する
2. client が session channel に `FILED_V2_OP_SESSION_DOORBELL` を 1 回送る
3. filed が ring を drain し、completion ring に結果を書く
4. filed が session channel に completion notification を 1 回送る
5. client が completion ring を読む

これで N 個の VFS operation を `2N syscall` から、おおよそ `2 syscall + shared memory polling` に落とせる。batch 数は固定しない。

ring full は normal session call に戻さない。client は doorbell して completion を回収するか、空きができるまで待つ。timeout / cancellation は control channel へ送る。fd passing と exec は control operation として ring protocol の外に置く。

single operation の latency をさらに落とすには、channel doorbell を event counter / futex / eventfd 相当へ置き換える必要がある。これは kernel notification primitive の話になるため、userland fast path で効果を見てから別途判断する。

#### Fast eligible operations

最初に fast path に載せる操作。

- `STAT`
- `PREAD` cache hit
- `PREAD` cache miss request enqueue
- `PWRITE` dirty cache hit
- `GETDENTS`
- `SET_FLAGS`
- `PING`

後続で ring に載せる操作。

- `OPENAT`: path walk と handle allocation があるため、ring descriptor 化はできるが completion 順序と handle id 発行を厳密にする
- `CLOSE`: lifetime operation だが、session fd table の `closing` state と completion ordering を入れて ring に載せる
- `DUP`: handle table mutation を伴うので completion ordering が必要

ring に載せない操作。

- `EXEC_PATH`: fd passing / process creation / bootstrap patch があるため control path のまま
- fd passing を伴う operation
- rights attenuation / handle export / debug / metrics

#### Ordering

同一 session 内では request id order を維持する。worker に投げた場合でも completion ring へ publish する順序は request order にする。将来 out-of-order completion を許す場合は flag で明示し、libc 側が request id lookup を持つ。

handle lifetime は filed 側が所有する。client が `CLOSE` を enqueue した後、その completion より前に同じ fd slot を再利用してはいけない。libc 側 fd table は `closing` state を持つ。

#### Metrics

fast path には operation-level とは別に ring metrics を持つ。

- `filed_fast_enqueued`
- `filed_fast_completed`
- `filed_fast_batches`
- `filed_fast_ring_full`
- `filed_fast_wait_ns`
- `filed_fast_dispatch_ns`

`hikitugi.md` には従来通り operation-level の数値だけを残す。ring metrics は調査ログに出す。

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

normal IPC は control plane である。trusted data operation は pkey/shared-memory data plane を標準経路にし、旧 data operation path は残さない。

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

pkey は trusted 別 process の高速 IPC として扱い、untrusted security boundary とはしない。untrusted process は別の normal IPC endpoint を使う。これは trusted path の互換経路ではない。

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

`filed` と seed/init/native daemon は trusted domain として pkey backend を使う。untrusted application や unknown process は別 endpoint の normal IPC control/data protocol を使う。

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

### Step 5: Coq abstract model first

- `verified/filed/spec` を作る
- `FiledTypes.v` に共通 id / rights / status / table model を置く
- `VfsModel.v` に mount / vnode / file / handle の抽象状態を置く
- `VfsSpec.v` に path walk / open / close / rights attenuation の invariant preservation を置く
- `ExecModel.v` に ELF image / mapping plan / stack plan / fd inherit plan を置く
- `ExecSpec.v` に `build_exec_plan` の基本仕様を置く
- まず VST は作らない
- Coq examples と C unit test が同じケースを通る状態を最初のゴールにする

### Step 6: filed receives FS backend endpoint

- `seed0root` または service table 経由で `filed` に `koboxd` FS backend endpoint を渡す
- debug 用に block endpoint も受け取れるようにする
- `filed` から `fs_mount_root` smoke を通す

### Step 7: ext4 backend mount through koboxd

- `filed` が `koboxd` FS backend endpoint 経由で ext4 rootfs を mount する
- `filed` は Linux inode/dentry/file を持たない
- `koboxd` が返す root backend object id から root vnode を作る

### Step 8: vnode lookup/read

- component lookup
- regular file open
- `pread`
- `statx`
- root directory `getdents`
- `pread` / `getdents` は `client <-> filed` data plane と `filed <-> koboxd` data plane を通す

### Step 9: libvfs IPC backend

- `libvfs` を追加する
- control plane と pkey/shared-memory data plane で `openat/pread/close/statx/getdents` を通す
- `seed0root` から `/sbin/koboxd.elf` または `/etc/pacha-release` を read する smoke を作る

### Step 10: filed exec service

- `filed` bootstrap に control endpoint fd を追加する
- `filed` が `EXEC_PATH` request を受け取る
- VFS model に従って path walk / read rights / execute rights を確認する
- Exec model に従って ELF validation / mapping plan / stack plan / fd inherit plan を作る
- C adapter が VMO create / process map / thread create / thread start を実行する
- 成功時は process fd / thread fd を caller に返す
- 失敗時は作成済み fd をすべて閉じる
- `seed0root` の bootstrap exec は `koboxd` と `filed` 起動用にだけ残す

### Step 11: client <-> filed pkey data plane

- trusted client 用 pkey ring setup
- normal IPC control message から ring を確立
- `pread` payload を pkey data plane に移す
- 旧 session data path は残さない

### Step 12: write path

- `pwrite`
- file offset update
- dirty file data handling
- `filed` cache と `koboxd` FS backend writeback policy の接続
- `fsync`
- basic write smoke

### Step 13: POSIX/musl integration

- musl file backend を `libvfs` に接続
- `open/read/write/close/lseek/stat/readdir/fsync` を通す
- stdio smoke を rootfs file に対して行う

### Step 14: filed <-> koboxd pkey data plane

- `fs_pread` / `fs_pwrite` / `fs_getdents` payload を pkey data plane に移す
- `filed` VFS semantics と `koboxd` Linux FS backend semantics を変えずに高速化する

### Step 15: RCU path walk

- race / stress test を増やす
- path walk read-side を RCU 化する
- rename / unlink / mount crossing との整合性を検証する
- lock-based path から RCU path へ置き換える

### Step 16: lock-free vnode cache

- vnode cache lookup を lock-free / mostly lock-free 化する
- cache miss / insert / eviction は既存 lock path で保守的に扱う
- negative dentry / negative lookup cache を入れる
- metadata snapshot に seqlock を導入する

### Step 17: per-worker cache

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

exec service の最初の成功条件。

```text
[filed] exec model examples OK
[filed] exec C unit cases OK
[seed0root] filed exec /sbin/seed0_next.elf plan OK
[seed0root] filed exec /sbin/seed0_next.elf started
```

ここで `seed0root` の bootstrap exec と `filed` の normal exec は分けて見る。`seed0root` は `koboxd` と `filed` を起動するための最小 loader を持つ。`filed` exec は rootfs path, rights, fd inheritance, argv/env/auxv を含む通常の process exec とする。

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
- 初期段階での VST proof
- `seed0root` bootstrap exec の完全削除

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
- `filed <-> koboxd` は FS backend service IPC とし、初期実装から pkey data plane を標準経路にする
- pkey backend は trusted 専用であり、untrusted isolation として扱わない
- `filed` は通常の pathname based process exec を提供する
- `seed0root` の exec は `koboxd` と `filed` を起動する bootstrap exception として残す
- VFS と exec は Coq 抽象モデルを先に作り、C 実装をその分解に合わせる
- 初期検証は VST ではなく、Coq executable model / invariant preservation / C differential test で行う
