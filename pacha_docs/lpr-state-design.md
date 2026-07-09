# LPR 状態モデル再設計案 (T3.3)

日付: 2026-07-10

目的: LPR の fd / process 周辺状態を Linux の fd セマンティクスに合わせて再設計する。既存挙動の保存ではなく、T3.0 残バグを診断・修正できる状態所有モデルへ移す。

非目的: kernel への機能追加、ABI 変更、lpr_supervisor のプロセス配置変更、実装コード変更。

## 1. 現状の問題の棚卸し

### 1.1 fd 状態が分裂している箇所

現行には `lpr_fd_table_*` という control table があるが、真の所有権は fd 種別ごとの shadow table に残っている。control table は同期対象であり、単一 fd table ではない。

| 種別 | 実体 | 主な定義箇所 |
|---|---|---|
| filed | `lpr_filed_fd_t { active, flags, handle, offset... }` / `lpr_fds` | `userland/personality/linux/runtime/lpr_filed_internal.h:123`, `userland/personality/linux/runtime/lpr_filed.c:9`, `userland/personality/linux/runtime/lpr_filed.c:15` |
| pipe | `lpr_pipe_fd_t { active, readable, writable, flags }` / `lpr_pipe_fds` | `userland/personality/linux/runtime/lpr_filed_internal.h:133`, `userland/personality/linux/runtime/lpr_filed.c:10`, `userland/personality/linux/runtime/lpr_filed.c:16` |
| eventfd | `lpr_event_fd_t { active, flags, counter }` / `lpr_event_fds` | `userland/personality/linux/runtime/lpr_filed_internal.h:141`, `userland/personality/linux/runtime/lpr_filed.c:11`, `userland/personality/linux/runtime/lpr_filed.c:17` |
| tty | `lpr_tty_fd_t { active, flags, handle }` / `lpr_tty_fds` | `userland/personality/linux/runtime/lpr_filed_internal.h:149`, `userland/personality/linux/runtime/lpr_filed.c:12`, `userland/personality/linux/runtime/lpr_filed.c:18` |
| socket | file-local `lpr_socket_fd_t lpr_sockets[128]` | `userland/personality/linux/runtime/lpr_socket.c:79`, `userland/personality/linux/runtime/lpr_socket.c:170` |
| control table | `lpr_fd_table_slot_t` + `lpr_fd_table_file_t` | `userland/personality/linux/runtime/lpr_fd/table.h:5`, `userland/personality/linux/runtime/lpr_filed.c:13`, `userland/personality/linux/runtime/lpr_filed.c:21` |
| fd array capacity | 6 配列を同時に mmap/realloc する layout | `userland/personality/linux/runtime/lpr_common/runtime_support.c:82`, `userland/personality/linux/runtime/lpr_common/runtime_support.c:124`, `userland/personality/linux/runtime/lpr_common/runtime_support.c:199` |

相互 negative check / 種別 cascade は以下に散っている。

| 用途 | 箇所 |
|---|---|
| active 判定関数 | filed/tty: `userland/personality/linux/runtime/lpr_fd/control.c:3`, `userland/personality/linux/runtime/lpr_fd/control.c:9`; pipe/eventfd: `userland/personality/linux/runtime/lpr_pipe/io.c:3`, `userland/personality/linux/runtime/lpr_pipe/io.c:319`; socket: `userland/personality/linux/runtime/lpr_socket.c:413` |
| local fd 判定 | `userland/personality/linux/runtime/lpr_fd/control.c:65` |
| native pipe 判定から local/socket を除外 | `userland/personality/linux/runtime/lpr_pipe/io.c:30`, `userland/personality/linux/runtime/lpr_pipe/io.c:41` |
| Linux visible 判定 | `userland/personality/linux/runtime/lpr_fd/control.c:459` |
| fd 割当時の空き判定 | `userland/personality/linux/runtime/lpr_fd/control.c:499` |
| socket 専用 fd 割当 | `userland/personality/linux/runtime/lpr_socket.c:425` |
| read/write/readv/writev/close/close_range/lseek/fcntl/flock/fstat | `userland/personality/linux/runtime/lpr_vfs/io.c:122`, `userland/personality/linux/runtime/lpr_fd/metadata.c:108`, `userland/personality/linux/runtime/lpr_fd/metadata.c:251`, `userland/personality/linux/runtime/lpr_fd/metadata.c:286`, `userland/personality/linux/runtime/lpr_fd/metadata.c:355`, `userland/personality/linux/runtime/lpr_fd/metadata.c:424`, `userland/personality/linux/runtime/lpr_fd/metadata.c:572`, `userland/personality/linux/runtime/lpr_fd/metadata.c:643` |
| dup/dup2 種別分岐と target close | `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:175`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:203`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:216`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:257`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:278`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:371`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:388` |
| syscall dispatcher の socket 特別扱い | `userland/personality/linux/runtime/lpr_dispatch.c:858`, `userland/personality/linux/runtime/lpr_dispatch.c:859`, `userland/personality/linux/runtime/lpr_dispatch.c:861`, `userland/personality/linux/runtime/lpr_dispatch.c:872`, `userland/personality/linux/runtime/lpr_dispatch.c:874`, `userland/personality/linux/runtime/lpr_dispatch.c:875`, `userland/personality/linux/runtime/lpr_dispatch.c:904`, `userland/personality/linux/runtime/lpr_dispatch.c:959` |
| poll/select の種別 cascade | `userland/personality/linux/runtime/lpr_socket.c:1217`, `userland/personality/linux/runtime/lpr_socket.c:1376` |
| exec/fork snapshot の種別 cascade | `userland/personality/linux/runtime/lpr_process/exec.c:186`, `userland/personality/linux/runtime/lpr_process/exec.c:239`, `userland/personality/linux/runtime/lpr_process/exec.c:401`, `userland/personality/linux/runtime/lpr_fd/dup_pipe.c:17` |
| bootstrap restore の種別 cascade | `userland/personality/linux/runtime/lpr_fd/control.c:552`, `userland/personality/linux/runtime/lpr_fd/control.c:572`, `userland/personality/linux/runtime/lpr_fd/control.c:588`, `userland/personality/linux/runtime/lpr_fd/control.c:612`, `userland/personality/linux/runtime/lpr_fd/control.c:628` |
| tty/eventfd poll guard | `userland/personality/linux/runtime/lpr_tty/runtime.c:255`, `userland/personality/linux/runtime/lpr_tty/runtime.c:396` |

詳細な行番号クラスタ:

- `userland/personality/linux/runtime/lpr_fd/control.c`: `3`, `9`, `15`, `21`, `26`, `65`, `189`, `256`, `279`, `296`, `313`, `326`, `343`, `356`, `413`, `432`, `459`, `499`, `552`, `572`, `588`, `612`, `628`。
- `userland/personality/linux/runtime/lpr_pipe/io.c`: `3`, `30`, `35`, `41`, `46`, `111`, `130`, `189`, `222`, `249`, `287`, `319`。
- `userland/personality/linux/runtime/lpr_fd/dup_pipe.c`: `3`, `17`, `99`, `175`, `197`, `203`, `216`, `257`, `278`, `310`, `364`, `371`, `388`, `399`, `414`。
- `userland/personality/linux/runtime/lpr_fd/metadata.c`: `3`, `108`, `174`, `251`, `286`, `355`, `424`, `497`, `543`, `572`, `588`, `643`, `710`, `761`, `900`, `974`。
- `userland/personality/linux/runtime/lpr_vfs/io.c`: `3`, `33`, `73`, `122`, `205`, `386`, `391`。
- `userland/personality/linux/runtime/lpr_vfs/ops.c`: `74`, `129`, `249`, `762`。
- `userland/personality/linux/runtime/lpr_socket.c`: `413`, `418`, `425`, `544`, `554`, `622`, `639`, `703`, `822`, `909`, `921`, `936`, `945`, `1008`, `1084`, `1108`, `1128`, `1145`, `1217`, `1376`。
- `userland/personality/linux/runtime/lpr_process/exec.c`: `186`, `194`, `212`, `239`, `263`, `273`, `401`。
- `userland/personality/linux/runtime/lpr_process/supervisor_fd_snapshot_glue.c`: `3`, `9`, `46`, `64`。
- `userland/personality/linux/runtime/lpr_tty/runtime.c`: `14`, `28`, `40`, `85`, `139`, `255`, `396`。
- `userland/personality/linux/runtime/lpr_dispatch.c`: `858`, `859`, `861`, `872`, `874`, `875`, `904`, `953`, `959`, `960`, `961`。

### 1.2 壊れうる遷移

- `dup2` の種別またぎ: target が filed/tty/pipe/eventfd/native pipe/socket のどれかを各所で判定し、`lpr_linux_close()` と native close の両方を試す (`lpr_fd/dup_pipe.c:364`-`421`)。shadow table と native fd が片方だけ残ると、次の判定順で別種別に見える。
- `dup/F_DUPFD`: eventfd は構造体コピー (`lpr_fd/dup_pipe.c:203`-`214`) で counter を複製しており、Linux の open file description 共有と一致しない。socket は `F_DUPFD` が `-ENOTSUP` (`lpr_socket.c:1100`-`1102`)。
- fork snapshot: child は fork 後に in-memory shadow table を持つが、filed/tty handle は child 側で個別 dup する (`lpr_fd/dup_pipe.c:17`-`63`)。この時点で fd entry、control table、daemon handle のいずれが正か追跡しづらい。
- execve 継承: supervisor 有効時は `lpr_supervisor_fd_table_replace()`、無効時は `lpr_prepare_exec_local_fds()` という別経路 (`lpr_process/syscalls.c:611`-`617`)。CLOEXEC 判定は `lpr_exec_local_fd_preserve()` 経由 (`lpr_process/exec.c:194`-`209`) で、socket 非 CLOEXEC は `-ENOTSUP` (`lpr_process/exec.c:229`-`233`)。
- close 漏れ: `lpr_linux_close()` は tty/eventfd/pipe/native pipe/filed の順に閉じる (`lpr_fd/metadata.c:251`-`284`)。socket は dispatcher が別経路へ逃がす (`lpr_dispatch.c:861`) ため、fd table の所有者が close の完全性を保証できない。
- close_range: local table と native fd を別ループで処理する (`lpr_fd/metadata.c:286`-`352`)。容量が `LPR_FD_TABLE_INITIAL_SIZE` に切られる native loop と、socket 固定 128 個の制約が混在する。
- filed offset: `lpr_fds[fd].offset` と control table offset の二重管理 (`lpr_fd/control.c:356`, `lpr_fd/control.c:413`)。read/readv cache 経路で更新されるが、dup/fork 共有 offset と衝突しうる。

### 1.3 T3.0 残バグで診断を阻む点

- バグ A: 16K+ パイプ停止。pipe fd が shadow table と native pipe の両方で扱われるため、停止時に「LPR が pipe と認識しているか」「kernel native pipe として残っているか」「poll/wait がどちらの状態を見ているか」を 1 回の dump で見られない。
- バグ B: `grep -q` 後停止。early reader exit では EPIPE/SIGPIPE、writer close、pipe EOF、shell の fd 継承が絡む。現構造では dup2 target close と close_range の漏れを fd 番号単位で追えない。
- バグ C: loader DT_NEEDED 空文字列。loader/ELF 側のバグの可能性は残るが、exec 直前の fd snapshot と file/readv/cache の状態が分裂しているため、読み取り元 fd/offset/cache 汚染と ELF parser の問題を分離しづらい。
- バグ D: 反復で OOM 劣化。fd close、VMO scratch、wire page、daemon handle、supervisor mirror のどこに参照が残ったかを fd lifecycle と同じ時系列で見られない。

## 2. 目標モデル

### 2.1 単一 fd table

LPR 内の Linux-visible fd は `lpr_state_t.fd_table` だけが所有する。全 syscall は最初に `lpr_fd_get(fd)` で `kind` を得て、kind 別 op に dispatch する。shadow table と相互 negative check は廃止する。

擬似構造:

```c
typedef enum {
    LPR_FD_NONE = 0,
    LPR_FD_FILED,
    LPR_FD_PIPE,
    LPR_FD_SOCKET,
    LPR_FD_EVENTFD,
    LPR_FD_TTY,
    LPR_FD_VMO,
    LPR_FD_NATIVE,
} lpr_fd_kind_t;

typedef struct {
    uint32_t fd_flags;      /* FD_CLOEXEC */
    uint32_t status_flags;  /* O_NONBLOCK, O_APPEND, access mode */
    uint32_t refcount;
    uint64_t generation;
    lpr_fd_kind_t kind;
    union {
        lpr_filed_payload_t filed;
        lpr_pipe_payload_t pipe;
        lpr_socket_payload_t socket;
        lpr_eventfd_payload_t eventfd;
        lpr_tty_payload_t tty;
        lpr_vmo_payload_t vmo;
        lpr_native_payload_t native;
    } u;
} lpr_fd_object_t;

typedef struct {
    lpr_fd_object_t *object; /* NULL means closed */
    uint32_t fd_flags;       /* per fd, not per object */
} lpr_fd_entry_t;

typedef struct {
    lpr_fd_entry_t *entries;
    uint32_t capacity;
    uint64_t generation;
    lpr_lock_t lock;
} lpr_fd_table_t;
```

`fd_flags` は fd entry に置く。`status_flags` と file offset は Linux の open file description 相当なので object に置く。ただし filed の offset 共有は filed 側 handle が open file description を表すことを前提にし、LPR の offset shadow は性能 cache としてだけ持つ。

### 2.2 ライフサイクル

所有者は fd table モジュール 1 つ。kind 別 module は payload の open/close/dup/serialize/poll/io callback だけを提供する。

| 遷移 | Linux 意味論 | PachaOS/LPR 対応 |
|---|---|---|
| open/openat | 最小未使用 fd を返す。`O_CLOEXEC` は fd flag。status flag は open file description。 | filed open 後、fd table に `FILED` object を install。失敗時は filed handle を即 close。 |
| pipe/pipe2 | 2 fd を同時作成。read end/write end は別 fd だが同じ pipe object の endpoint。`O_CLOEXEC` は両 fd。 | kernel pipe fd を payload に保持。install は 2 entry atomic。片方失敗なら両方 close。 |
| socket | fd 番号は通常 fd table から割当。`SOCK_CLOEXEC` は fd flag、`SOCK_NONBLOCK` は object status。 | `lpr_sockets[128]` を廃止し、netd handle と socket state を payload に持つ。 |
| eventfd | dup/fork 後は同じ eventfd counter を共有。 | counter は object payload。entry copy では複製しない。 |
| dup/F_DUPFD | 新 fd は同じ open file description を参照。CLOEXEC は clear、`F_DUPFD_CLOEXEC` は set。 | object refcount++、entry fd_flags を指定値で作成。 |
| dup2 | `oldfd` が有効なら `newfd` を暗黙 close して同じ object を指す。`oldfd==newfd` は no-op。 | fd table lock 下で close callback と refcount 操作を順序化。`dup3(old,new,O_CLOEXEC)` だけ `old==new` は `EINVAL`。 |
| dup3 | dup2 + flags。`oldfd==newfd` は `EINVAL`。 | syscall wrapper で差分を判定し、fd table primitive は共通化。 |
| fcntl(F_DUPFD*) | `arg` 以上の最小空き fd。 | fd table allocator のみ使用。socket も同じ経路で対応。 |
| close | fd entry を消す。最後の参照なら object close。 | entry を先に closed にし、callback 失敗は trace する。Linux close は fd 番号を再利用可能にするため、close 後エラーで復活させない。 |
| close_range | 範囲内 fd に close または CLOEXEC set。 | fd table lock 下で generation を 1 回進める。`UNSHARE` は T4.1 まで no-op/未対応を明示。 |
| fork snapshot | fork 後、親子は open file description を共有。CLOEXEC は適用しない。 | kernel native fd clone に任せられる kind は retain。daemon handle は kind callback の `fork_dup` で同じ open file description を指す handle を作る。表現不能なら filed offset shadow を invalid にして filed を真にする。 |
| execve 継承 | exec 成功時だけ CLOEXEC fd を閉じ、非 CLOEXEC fd は継承。 | exec commit 直前に fd table snapshot を作成し、bootstrap へ渡す。CLOEXEC 適用は snapshot 作成時。exec 失敗時は元 fd table を変更しない。 |

### 2.3 lpr_supervisor との境界

原則 1 に従い、lpr_supervisor の常時 fd ミラーを廃止する。現行の `lprs_filedesc_t` は `main.c:39`-`46`、replace begin/chunk/commit は `main.c:828`-`870`、get chunk は `main.c:873`-`901` にあるが、これは source of truth にしない。

新境界:

- LPR が fd table の唯一の所有者。
- lpr_supervisor は process/session/job-control metadata を持つ。fd は fork/exec の瞬間に必要な snapshot payload を一時保持するだけ。
- snapshot は `begin { token, generation, total_count }`、`chunk { start_index, entries[] }`、`commit { generation, crc }` の 3 段階。commit 済み generation だけ restore 可能。
- snapshot 中は fd table lock を保持するか、copy-on-snapshot buffer を作る。途中で fd table generation が変わったら abort/retry。
- exec 用 snapshot は filed/bootstrap に直接渡せるなら supervisor を経由しない。supervisor 経由が必要な場合も、保存期間は「新 runtime が restore 完了するまで」に限定する。

snapshot entry:

```c
typedef struct {
    uint32_t fd;
    uint32_t kind;
    uint32_t fd_flags;
    uint32_t status_flags;
    uint64_t object_id;          /* debug only, restore identity hint */
    uint64_t handle_or_native_fd;
    uint64_t offset_or_counter;  /* kind が明示的に必要な場合だけ */
    uint64_t rights;
    uint64_t generation;
} lpr_fd_snapshot_entry_t;
```

### 2.4 状態の集約

file-scope static を `lpr_state_t` に集約する。最低限の構成:

```c
typedef struct {
    lpr_fd_table_t fd_table;
    lpr_process_state_t process;
    lpr_signal_state_t signal;
    lpr_cwd_state_t cwd;
    lpr_rlimit_state_t rlimits;
    uint64_t umask_value;
    lpr_rpc_state_t filed_rpc;
    lpr_rpc_state_t termd_rpc;
    lpr_rpc_state_t netd_rpc;
    lpr_cache_state_t caches;
    lpr_debug_state_t debug;
} lpr_state_t;
```

`lpr_filed.c:25`-`74` の readlink/page cache、wire page、process、signal、cwd、supervisor token はここへ移す。socket file-local static (`lpr_socket.c:170`-`175`) も `lpr_state_t` 配下に入れる。

### 2.5 lock 設計

T4.1 の `CLONE_THREAD` を見据えるが、T3.3 では single-thread fast path を保つ。

- fd table は futex ベースの 1 本 lock を基本にする。理由: `dup2`、`close_range`、snapshot は複数 entry を atomic に見る必要があり、entry 単位 lock では lock ordering が複雑になる。
- `lpr_state_t.thread_count == 1` の間は lock/unlock を no-op にする。CLONE_THREAD 成功時に thread_count を 2 以上へ遷移させ、その後 futex lock を有効化する。
- kind payload 内に長時間 RPC が必要な場合、fd table lock 下では entry/object の参照だけ retain し、lock を外して RPC する。最後の close callback は object refcount が 0 になった後に実行する。
- entry 単位 lock は T4.1 後、poll/io 高頻度 path の実測 contention が出た時だけ導入する。

### 2.6 可観測性

`lpr_state_dump(reason)` を最初から設計に含める。出力は pacha/trace のイベント列で、1 回の dump が fd table 全体を再構成できることを条件にする。

イベント案:

- `lpr.state.begin(pid, generation, fd_capacity, open_count, reason)`
- `lpr.fd.entry(fd, kind, fd_flags, status_flags, object_id, object_refcount)`
- `lpr.fd.filed(fd, handle, offset_shadow, offset_valid, cache_generation)`
- `lpr.fd.pipe(fd, native_fd, readable, writable, pipe_id)`
- `lpr.fd.socket(fd, handle, connected, connecting, last_error)`
- `lpr.fd.eventfd(fd, counter)`
- `lpr.fd.tty(fd, handle, foreground_pgrp)`
- `lpr.state.end(generation, crc)`

起動方法案:

- debug syscall: `pachaos_debug(LPR_DEBUG_DUMP_STATE, reason)`。ABI 追加が必要なら T3.3 実装前に理由を明文化する。
- ABI 追加を避ける代替: tty から `ESC ] pacha:lpr-dump BEL` の特殊制御列を termd が LPR へ通知する。T3.3 では提案に留め、最初は既存 trace hook から呼べる内部関数として実装する。

## 3. 移行計画

### Step 1: `lpr_state_t` と dump の土台

- `lpr_state_t` を導入し、既存 static への alias/wrapper として接続する。
- `lpr_state_dump()` は既存 shadow table を読むだけにする。
- 受け入れ: 既存 green smoke は green 維持。red smoke は同じ red。dump に fd 0/1/2、pipe/eventfd/tty/filed の既存状態が出る。

### Step 2: fd table module を唯一の判定元にする

- filed/pipe/eventfd/tty を `lpr_fd_entry_t` 経由へ移す。
- `lpr_fd_is_filed()` などの active 関数は fd table lookup wrapper に置換し、shadow table は payload へ吸収する。
- `dup/dup2/dup3/F_DUPFD/close/close_range/fcntl` を fd table primitive に集約する。
- 受け入れ: fd-pipe/ext4/既存 smoke green。pipe-stress CASE1/2/5/6/7 は green 維持または改善。CASE3/4 が red の場合は dump で該当 pipe fd の reader/writer/refcount/wait 状態が説明できる。

### Step 3: socket/native/bootstrap/supervisor snapshot 統合

- socket 固定配列を fd payload に移す。
- native pipe fallback を fd table install 時に吸収し、「native だが LPR-visible」の曖昧状態をなくす。
- exec/fork/bootstrap/supervisor snapshot を `lpr_fd_snapshot_entry_t` に統一する。
- 受け入れ: GNUCU_CASE2 (`grep -q`) が green、または fd dump で close/EPIPE/SIGPIPE のどこで止まったか説明できる。socket fcntl dup が Linux と同じになる。

### Step 4: lock と状態集約の完了

- fd table global futex lock、single-thread fast path、snapshot generation 検証を入れる。
- rlimits/umask/cwd/signal/cache/RPC scratch を `lpr_state_t` 配下へ移す。
- 受け入れ: pipe-stress 全ケース x5、gnu-coreutils 全ケース、fd-pipe/ext4 が green。反復 smoke で open_count/live_object_count が開始値へ戻る。ベンチは ±10% 以内。

## 4. T3.0 バグとの対応

### バグ A: 16K+ パイプ停止

仮説: writer が pipe capacity 超えで wait した後、reader close/EOF/wakeup のどれかが LPR shadow table と native pipe state の不一致で見えなくなる。

新モデルで見える場所:

- `lpr.fd.pipe` の reader/writer endpoint、object refcount、native fd、last wait event。
- `dup2`/`close_range` の trace と fd generation。

モデル自体が直す可能性: 高い。pipe fd が shadow/native fallback の二重判定から消え、dup2 target close が atomic になるため、孤児 endpoint や誤 POLLNVAL が減る。

### バグ B: `grep -q` 後停止

仮説: `grep -q` が早期 exit した後、上流 writer への EPIPE/SIGPIPE または shell の pipe close が欠落して待ち続ける。

新モデルで見える場所:

- pipeline 各 fd の refcount、CLOEXEC、fork 継承 snapshot。
- close 時に最後の writer/read end が閉じたか、SIGPIPE を発火したか。

モデル自体が直す可能性: 中から高。close/dup2/fork 継承が fd table に集約されるため、残留 writer の診断と修正がしやすい。

### バグ C: loader DT_NEEDED 空文字列

仮説: ELF parser 自体、file read/cache、exec snapshot のいずれか。現時点では LPR fd モデル原因と断定しない。

新モデルで見える場所:

- exec 直前の fd snapshot、対象 executable/interpreter の filed handle、offset shadow invalidation、readv/pread 経路。
- `lpr_state_dump("execve_begin")` と loader trace を同じ generation で結べる。

モデル自体が直す可能性: 低から中。ELF parser の範囲チェックが原因なら直らない。ただし fd offset/cache 汚染が原因なら、offset 所有者を filed/open file description に寄せることで改善する可能性がある。

### バグ D: 反復で OOM 劣化

仮説: fd/object/VMO/wire page/daemon handle のいずれかが反復 exec/fork/pipe で release されず残る。

新モデルで見える場所:

- fd table open_count、object live_count、kind 別 close callback 結果。
- exec 失敗時・成功時の snapshot VMO、wire page、bootstrap fd の所有権。

モデル自体が直す可能性: 中。kernel VMO lifetime 欠陥は T1.1 領域なので T3.3 だけでは直らない可能性がある。一方、LPR の fd/handle/wire page 漏れは owner が 1 つになるため直る可能性がある。

## 5. 設計上の決定 (2026-07-10 ユーザー承認)

1. **filed offset の所有者 = filed**。filed 側 handle が open file description として offset を所有し、fork/dup 後の共有 offset を Linux と一致させる。LPR 側 offset は無効化可能な性能 cache に限定する。
2. **snapshot 転送先 = exec bootstrap VMO へ直接**。supervisor は process/session/job-control metadata のみ持ち、fd snapshot の transport にしない (原則 1)。
3. **dump 起動 = 内部関数から開始**。`lpr_state_dump()` は既存 trace hook から呼べる内部関数として実装。外部トリガ (debug syscall / tty 特殊列) は必要になった時点で判断。
