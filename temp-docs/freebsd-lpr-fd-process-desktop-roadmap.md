# FreeBSD型 LPR FD/Process と bash/coreutils -> clang -> Mesa/DE ロードマップ

## 目的

この計画の目的は、個別の `fcntl` や pipe バグをその場で潰すことではなく、
バグが構造的に起きやすい状態を減らすことである。

中期目標は次の順で進める。

1. bash と coreutils が落ちにくく、pipe/redirection/fork/exec/wait が
   デバッグしやすい状態にする。
2. CLI 負荷テストとして clang / clang++ を再び動かす。
3. llvmpipe と Mesa を足場に、軽量な Wayland desktop environment と
   小さい Wayland application を動かす。

設計上の強い参考元は FreeBSD の process / file descriptor / file /
session / process group の責務分離である。ただし実装は PachaOS の
userland service 構造に合わせる。

## 基本方針

- userland 中心で直す。
- kernel は今回の主対象にしない。
- kernel を触る場合は、userland で解けない根拠を先に作る。
- kernel 側を変更する場合は、コメントを多めに入れ、関数名だけに説明を
  押し込まない。
- 互換維持より構造の明確さを優先する。
- v2 ABI は破壊的に更新してよい。旧 ABI shim は作らない。
- pipe は当面 kernel primitive として維持する。
- Linux / UNIX fd 意味論は LPR runtime と lpr_supervisor 側で整理する。

## 現在見えている構造的な不安定さ

`lpr_filed.c` には、fd/proc/signal/exec/pipe/tty/filed client の責務が
混ざっている。

特に不安定化しやすい点。

- fd-local flags と open file description state が per-fd 配列に混在している。
- `lpr_fds[]`, `lpr_pipe_fds[]`, `lpr_tty_fds[]`, `lpr_event_fds[]` が
  同じ Linux fd table の複数の真実になっている。
- `dup` / `dup2` / `fcntl(F_DUPFD)` が fd-local flags と shared offset を
  一貫して扱いにくい。
- exec inheritance と lpr_supervisor fd table snapshot が drift しやすい。
- process tree / pgrp / session / signal / wait が LPR runtime と
  lpr_supervisor に分散している。
- `poll` / `select` は 10ms sleep polling が残っていて、負荷時の race や
  latency を隠しやすい。
- 長い 1 行を virtio-console に流す test は入力経路自体が不安定なので、
  stress test の形として信用しすぎない。

直近の赤症状。

```sh
busybox sh -c "echo z >/tmp/f; exec 8</tmp/f; read -r -n 2 a <&8; read -r -n 2 b <&8; echo ${a}${b}"
```

この系で `sh: fcntl(8,F_DUPFD,10): Invalid argument` が出る。
これは `F_DUPFD` と fd table 状態 drift の最初の回帰テストにする。

## FreeBSD型の恒久構造

PachaOS の LPR では、FreeBSD の `proc -> filedesc -> file` に相当する
構造を次のように置く。

```text
lpr_supervisor
  lprs_proc
    pid / ppid / process_fd / lifecycle / wait status
    filedesc reference
    session reference
    pgrp reference

  lprs_filedesc
    Linux fd number -> descriptor entry
    fd-local flags: CLOEXEC
    file reference

  lprs_file
    kind: filed / tty / pipe / event / socket / native
    status flags: NONBLOCK / APPEND / SYNC
    offset
    backend reference
    refcount

  lprs_session
    sid
    controlling tty
    foreground pgrp

  lprs_pgrp
    pgrp id
    member process set
    signal delivery target
```

LPR runtime は syscall translation と高速 cache を担当する。

```text
LPR runtime
  lpr_fd_table        Linux fd API facade / local cache
  lpr_vfs_client      filed client
  lpr_tty_client      termd client
  lpr_process_client  lpr_supervisor client
  lpr_signal_client   signal translation
  lpr_pipe_event      kernel pipe/event/timer fd wrapper
```

長期的な authority は lpr_supervisor に寄せる。
LPR runtime の `lpr_linux_current_pid/sid/pgrp` は authority ではなく cache とする。

## ABI 更新方針

`lpr_supervisor/ipc_protocol_v2.h` は破壊的に更新する。

必要な方向。

- fd kind に `PIPE`, `SOCKET`, `NATIVE` を追加する。
- fd table payload を fd-local flags と file status flags に分ける。
- process payload を proc / filedesc / session / pgrp / ctty / cwd に分ける。
- signal payload は process signal と tty-generated signal を分ける。
- diagnostics op で proc/session/pgrp/fd table を dump できるようにする。

旧 v2 layout の互換 shim は作らない。
全 client/server を同時に更新する。

## 実装フェーズ

### Phase 1: FD control を新方式へ接続

目的は bash/coreutils の足場になる fd 意味論を安定させること。

- `lpr_fd_table` を実経路へ接続する。
- `F_DUPFD`, `F_DUPFD_CLOEXEC`, `dup`, `dup2`, `close_range`, `F_GETFD`,
  `F_SETFD`, `F_GETFL`, `F_SETFL` を新 fd table API 経由に寄せる。
- fd-local flags と file status flags を分ける。
- `dup` 後の offset/status flag 共有を固定する。
- per-kind 配列直叩きを fd control 経路から外す。

最初の合格条件。

- `fcntl(8,F_DUPFD,10)` が `EINVAL` にならない。
- `read -n` 系 redirection が通る。
- host smoke と QEMU smoke の両方で `F_DUPFD` を確認する。

### Phase 2: lpr_supervisor process model を分離

目的は process/session/pgrp/wait/signal の単一 authority を作ること。

- `lprs_process_t` を分割する。
- `lprs_proc`, `lprs_filedesc`, `lprs_file`, `lprs_session`, `lprs_pgrp` を導入する。
- fork は parent の filedesc を refcount 付きで共有する。
- exec は CLOEXEC を適用した filedesc snapshot を作る。
- wait は process lifecycle state を見て決定する。
- signal は process / pgrp / tty-generated signal の経路を分ける。

### Phase 3: LPR runtime module split

目的は `lpr_filed.c` を syscall dispatcher と client glue へ縮小すること。

- `lpr_vfs_client`
- `lpr_tty_client`
- `lpr_process_client`
- `lpr_signal_client`
- `lpr_pipe_event`
- `lpr_error`

service-private headers は LPR runtime の各所へ散らさない。
LPR runtime が直接見る ABI は `personality/lpr_client_abi.h` と narrow helper に寄せる。

### Phase 4: bash/coreutils 安定 gate

ここで bash と coreutils を日常的な smoke として使える状態にする。

必須 smoke。

- `echo`, `cat`, `wc`, `head`, `tail`, `true`, `false`, `test`
- `echo x | cat`
- `printf abc | wc -c`
- `cat file | grep`
- `2>&1 | grep`
- `yes | head -n 3 | wc -l`
- `<`, `>`, `>>`, `2>&1`
- `exec 8</tmp/f; cat <&8`
- `read -n` redirection
- fork/wait loop
- pipe/open/close/exec loop

stress は長い 1 行を virtio-console に直接投げない。
短いコマンド列、または rootfs 上の script file を実行する方式にする。

### Phase 5: clang / clang++ CLI 負荷 gate

目的は llvmpipe/Mesa 前の CLI 負荷テストを復帰すること。

- clang / clang++ binary と必要な runtime/library を rootfs に置く。
- 小さい C file の preprocess / compile / assemble 相当を確認する。
- 複数 file compile で fd leak、pipe wait、exec wait を観測する。
- `fork`, `execve`, `wait4`, `pipe`, `stat`, `openat`, `mmap`, `readlink`,
  `getdents64` の不足を洗う。

この gate は performance より構造バグ検出を優先する。

### Phase 6: Mesa / llvmpipe / lightweight DE gate

目的は GUI stack に必要な userland 境界を洗い出すこと。

- llvmpipe が要求する thread, mmap, file, futex 相当の不足を確認する。
- Mesa loader が必要とする path, fd, dlopen/dynamic loader error を診断可能にする。
- Wayland compositor または小さい test compositor を起動する。
- lightweight DE は最後の統合 gate とする。

この段階までに fd/process/signal/wait の debug dump が使える状態にしておく。

## Kernel 方針

kernel は次だけを持つ。

- fd capability table
- rights
- IPC fd passing
- process/thread primitive
- process fd inheritance primitive
- kernel pipe byte-stream / wait primitive
- eventfd/timerfd/vmo/endpoint

kernel に寄せないもの。

- Linux fd flag 意味論
- POSIX path/file policy
- exec 時の Linux fd table 構築
- TTY session/pgrp policy
- Linux process tree policy

kernel を変更する場合。

- userland だけでは race / atomicity / wait guarantee を満たせない証拠を作る。
- 変更前に理由を明文化する。
- コメントで invariant と boundary を書く。
- 説明的すぎる関数名に頼らず、短い名前 + コメントで意図を残す。

## テストと診断

常時回す基本テスト。

```sh
tests/run-lpr-fd-table-tests.sh
tests/run-userland-service-abi-layout.sh
userland/filed/tests/run-vfs-tests.sh
.artifacts/bin/pacgo sync rootfs --force
.artifacts/bin/pacgo sync bootfs
tests/run-lpr-qemu-fd-pipe-smoke.sh
```

`tests/run-lpr-qemu-fd-pipe-smoke.sh` は長い 1 行を `qemu-test --send` で
一括投入しない。Python console driver が短い smoke と長めの smoke を分けて送り、
長い command は文字ごとの遅延と Enter 前の待ちを入れて TTY 側の入力取りこぼしを
避ける。pipe smoke は入力 echo ではなく実出力行を期待する。成果物 sync 済みで
再実行する場合だけ `SKIP_SYNC=1 tests/run-lpr-qemu-fd-pipe-smoke.sh` を使う。

追加する診断。

- LPR fd table dump
- lpr_supervisor proc/session/pgrp/fd dump
- filed exec inheritance dump
- termd tty/session dump
- dynamic loader failure dump

失敗時には最低限これを出せるようにする。

- service id
- op
- trace id
- pid / token
- fd
- fd kind
- fd-local flags
- file object id
- backend reference
- error domain

## Definition of Done

- `F_DUPFD` / `read -n` 赤症状が再発しない。
- bash/coreutils pipe/redirection smoke が 30 秒 QEMU test で通る。
- fd table と open file description の責務がコード上で分かれている。
- process/session/pgrp/wait/signal の authority が lpr_supervisor に寄っている。
- clang / clang++ の小さい compile smoke が通る。
- llvmpipe/Mesa/Wayland の不足が診断可能な形で見える。
- kernel に Linux userland policy が増えていない。
