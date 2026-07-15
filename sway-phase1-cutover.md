# Sway Phase 1 Cutover

## 1. 目的

Phase 1 の目的は、TTY の通常 shell から upstream `/usr/bin/sway` を直接起動し、
Sway から upstream Foot とその PTY shell を利用できる状態を作ることである。

完了時には次を満たす。

- Sway の process tree に専用 launcher がいない。
- `LD_PRELOAD`、`LP_NUM_THREADS=0`、固定 10 ms poll を使わない。
- first frame は実際の output commit と page flip で確認する。
- normal exit、SIGTERM、SIGKILL 後に process、fd、service resource が baseline へ戻る。
- kernel ABI は増やさず、Linux personality と userland service の責務として完成させる。

`userland/fixtures/src/wsl_musl/lpr_sway_launcher.c`、その build entry、既存 fixture は
この Phase では編集も削除もしない。新しい製品経路と受け入れ試験から呼ばない。

## 2. 固定する判断

- T3.3 で導入した単一 fd table、entry/object 分離、refcount、dup 基盤は再利用する。
- table を全面再実装せず、object/backend 境界、transaction、wire、wait を切り替える。
- private ABI は producer と consumer を同じ change で更新し、旧 reader/writer を残さない。
- 型、file、symbol に世代 suffix、`old`、`new`、`legacy` を付けない。
- ABI version は数値だけを進める。`LPR_IMAGE_ABI_VERSION` は 11 とする。
- private operation enum は意味順に並べ直し、変更前の番号を維持しない。
- service lease は中央 broker を作らず、共通契約を使う各 service の台帳で管理する。
- session lifetime は `/usr/libexec/pacha-user-session` が所有する。
- wlroots の同期変更は renderer 一般の契約として実装し、PachaOS や Sway の名前で分岐しない。
- kernel の既存 worktree change は保全し、この計画から新しい kernel 編集を行わない。

## 3. 完成時の fd モデル

### 3.1 entry / OFD / backend

既存 storage と allocator を使い、名称と責務を次へ揃える。移行用 typedef は残さない。

```c
typedef struct lpr_fd_entry {
    uint32_t ofd_index;
    uint32_t ofd_generation;
    uint16_t fd_flags;          /* FD_CLOEXEC */
    uint16_t state;             /* FREE / RESERVED / OPEN */
    uint64_t effective_rights;
} lpr_fd_entry_t;

typedef struct lpr_ofd {
    uint32_t refcount;
    uint32_t pin_count;
    uint64_t generation;
    uint32_t access_mode;
    uint32_t status_flags;
    uint64_t offset;
    lpr_backend_ref_t backend;
} lpr_ofd_t;
```

- `lpr_fd_table_slot_t`、`lpr_fd_table_file_t`、`lpr_fd_object_t` は削除する。
- OFD の巨大 kind union を削除し、backend は `{ops_id, slot, generation}` で参照する。
- backend-private state は各 backend arena が所有する。
- common flags、handle、native wait fd を backend payload に複製しない。
- lookup は pin または snapshot を返し、table unlock 後に raw pointer を残さない。
- table lock 中に RPC、backend callback、wait、mmap を行わない。
- dup は entry だけを増やし、最後の OFD ref が backend を一度だけ close する。

### 3.2 backend ops

registry は少なくとも次の operation を持つ。

- read/write/ioctl/stat/mmap
- poll query、wait source 収集、wake 後 recheck
- fork、exec、transfer の prepare/commit/rollback
- export/import/claim/cancel
- final close と service HUP

unsupported operation は `EOPNOTSUPP`、壊れた generation は `EBADF` とし、
別 kind や native fd へ fall through しない。

### 3.3 Linux fd と native fd

- `lpr_linux_fd_t` は fd table index、`lpr_native_fd_t` は unwrap 制限付き struct とする。
- Linux syscall は table miss 時に同じ整数の native fd を探さない。
- native fd は backend または runtime-private registry だけが所有する。
- stdin/stdout/stderr は bootstrap 時に TTY backend entry として明示的に install する。
- service endpoint、wire page、wait channel は Linux fd number と独立させる。
- fork 後は kernel が複製済みの native refs を child transaction が adopt/drop する。

## 4. fork、exec、bootstrap

### 4.1 共通 transaction

fork、exec、transfer は一意な transaction id と次の状態機械を共有する。

1. table を snapshot し、unique OFD を pin する。
2. lock を外し、各 backend/service で prepare する。
3. 全 prepare 成功後だけ visible state を commit する。
4. 失敗時は逆順 rollback する。
5. commit/rollback は同じ transaction id に対して idempotent とする。
6. client death は process-liveness fd の HUP で未完了 transaction を回収する。

### 4.2 exec manifest

LPR が一つの bootstrap VMO を生成し、次を格納する。

- process、signal、cwd、session metadata
- logical fd entries と OFD records
- backend id、opaque backend record、native capability ordinal
- record bounds、count、generation、全体 checksum

Filed はこの VMO の中身や fd kind を解釈しない。ELF/address-space の staging、
bootstrap VMO と capability attachments の配送、`PROCESS_EXEC_FROM` だけを担当する。

`FILED_EXEC_LPR_FD_TABLE`、`filed_exec_lpr_fd_t`、kind 別 bootstrap descriptor、
Filed 内の LPR fd validation/copy を削除する。CLOEXEC は LPR が manifest 作成時に適用し、
exec 失敗時は元 table と service lease を変更しない。

seed0root は同じ builder library で最初の session bootstrap を作る。Filed 専用の
default-stdio 知識は削除し、初回起動と通常 exec を同じ manifest 形式へ揃える。

## 5. service lease と SCM_RIGHTS

### 5.1 service-local lease

LPRS は process token、generation、waitable process-liveness fd を発行する。
filed、netd、drmd、inputd、termd は最初の request で owner を attach し、
自分の resource table に次の ref class を記録する。

- PROCESS: process が直接所有する handle
- TRANSFER: queue または receiver claim 待ち
- MAPPING: fd close 後も残る mmap
- INFLIGHT: RPC と fork/exec transaction

HUP 時に PROCESS と INFLIGHT を即時回収する。TRANSFER と MAPPING はそれぞれの owner が
残る限り保持する。次の open、reconnect、cache eviction を cleanup trigger にしない。

### 5.2 opaque transfer

SCM_RIGHTS wire は fd kind を持たず、各 occurrence を次で表す。

```text
provider id | transfer token | rights | fd flags | capability ordinals
```

- netd は capsule bytes、attachments、queue ownership だけを扱う。
- send は provider prepare 後に queue lease を作り、enqueue 失敗時に cancel する。
- receive は全 item を prepare/import し、全 Linux fd allocation 成功後に一括 claim する。
- 途中失敗は新 entry、attachments、provider ticket をすべて rollback する。
- multiple fd、A→B→C、dup、CLOEXEC、rights attenuation、mapping-after-close を扱う。
- sender/receiver/queue の各 death で double release と dangling handle を残さない。

## 6. wait graph、child、signal

各 backend は ready bits、native wait leaves、任意の absolute deadline を返す。

- epoll は nested epoll を再帰的に flatten し、cycle と generation を検査する。
- 一度の `FD_WAIT_MANY` で native leaves と最短 deadline を待ち、wake 後に全対象を recheck する。
- level-trigger は毎回再評価し、edge-trigger は readiness generation を記録する。
- eventfd は counter の 0→nonzero、timerfd は deadline、service は HUP を wait source にする。
- pipe、socket、input、DRM completion は backend の native leaf を使う。
- 固定 quantum、sleep retry、non-native を理由にした周期確認を削除する。

LPRS は parent ごとの child-event source と monotonic sequence を持つ。child state は exact PID が
reap されるまで保持する。SIGCHLD は通常の pending signal、signal frame、restorer を通し、
LPR C code から handler を直接呼ばない。現在の child 用 10 ms sampling 削除は保全し、
generic epoll と socket に残る 10 ms fallback もこの切替で削除する。

## 7. Sway/Foot に必要な backend と Linux semantics

次の順で backend を最終 ops へ移し、移した backend の旧 accessors と kind branch を同じ patch で消す。

1. process/child event、TTY/PTTY、pipe/eventfd/timerfd
2. Filed regular file、tmpfs、memfd、mmap、seals
3. local socket、epoll、SCM_RIGHTS
4. DRM、dma-buf、sync-file、input

同時に `/dev/shm`、sparse tmpfs、fallocate/punch-hole、getrandom、passwd/group、
動的 devpts、`TIOCGPTN`、PTY index 回収を完成させる。working implementation は保持できるが、
最終 backend ops を迂回する入口は残さない。

wlroots keymap は通常の `memfd_create`、mmap、SCM_RIGHTS、close lifetime を使う。
keymap preload は新 session と direct Sway test から削除する。

## 8. threaded llvmpipe の producer/consumer 同期

wlroots 0.18.2 GLES2 pass は `glFlush()` 後に buffer を渡す。一方 Mesa 25.1.9 llvmpipe は
`glFlush()` で raster work を enqueue するが完了待ちはしない。page flip 側だけでは producer 完了を
推測できないため、固定 delay や drmd 内 sleep では直さない。

### 8.1 sync-file backend

- Linux sync-file fd を pollable backend として実装する。
- `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` は現在の producer completion を表す fd を返す。
- `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` は dma-buf の次の consumer acquire 条件へ attach する。
- completion 未登録の export は signaled fd、未完了は signal 時だけ readable とする。
- dup、SCM_RIGHTS、exec、close、owner death は通常の OFD/lease 契約を使う。

これにより Mesa llvmpipe の `lp_fence_get_fd()` が利用する dummy dma-buf export と
`EGL_ANDROID_native_fence_sync` を有効にする。llvmpipe は fence fd export 時に raster 完了を待ち、
以後は signaled sync-file を返す。

### 8.2 generic wlroots patch

- GLES2 pass submit 後に EGL native fence fd を取得し、描画先 `wlr_buffer` の acquire fence とする。
- buffer は fence fd を一つ所有し、新しい submit、consume、destroy で厳密に close する。
- DRM backend は atomic IN_FENCE_FD があれば渡し、なければ page flip 前にその fd を一度だけ待つ。
- pixman は synchronous completion、Vulkan は既存 timeline から同じ buffer contract を供給できる形にする。
- native fence 非対応 renderer は従来の platform implicit sync を使うが、PachaOS software path は
  fence extension が無ければ明示エラーにして unsynchronized scanout を行わない。

patch は renderer/backend 一般の変更として独立 test を付け、Sway source、process 名、環境変数を
参照しない。`glFinish` の常時挿入は行わず、producer から consumer への handoff でだけ待つ。

## 9. session bootstrap と direct Sway

seed0root は Bash の代わりに `/usr/libexec/pacha-user-session` を起動する。
session process は次を行う。

- `/run/user/0` を mode 0700 で作成し、`XDG_RUNTIME_DIR=/run/user/0` を設定する。
- seatd を `/run/user/0/seatd.sock` で起動し、socket connect 成功を readiness とする。
- `LIBSEAT_BACKEND=seatd`、`SEATD_SOCK`、`SEATD_VTBOUND=0` を export する。
- `WLR_RENDERER_ALLOW_SOFTWARE=1` と通常の PATH、HOME、TERM、library path を設定する。
- 対話 Bash を foreground process group として起動し、seatd は session lifetime まで維持する。
- shell 終了時は seatd を SIGTERM、wait、必要時だけ SIGKILL の順で回収する。

shell から `exec /usr/bin/sway -c /etc/sway/config` または `sway` を実行する。
session は Sway socket、first frame、client 起動を待たず、Sway 固有 cleanup も行わない。

## 10. 実施順と作業分担

一つの Root turn で一つの Step を終端まで進める。isolated helper や一種類の backend だけを
完了報告にせず、Step 内の producer/consumer 切替、旧経路削除、build 可能化までを一単位とする。

### Step 1 — fd core cutover

- 既存 table storage を canonical entry/OFD/backend registry へ切り替え、Linux/native namespace、pin/snapshot、final-close、dup semantics を一括適用する。
- 全 constructor と central dispatch を ops 経由へ移し、kind union、旧 typedef/accessor、native fallback を削除する。終端は関連 target の build と旧 symbol 0 件。

### Step 2 — lifetime and wire cutover

- fork/exec と初回起動を opaque manifest transaction へ揃え、Filed の fd kind 知識と kind 別 exec descriptor を削除する。
- 全 service に lease を適用し、SCM_RIGHTS を multi-fd opaque transaction へ移す。終端は全 userland build、orphan reap・旧 wire symbol 0 件。

### Step 3 — wait and Linux semantics cutover

- wait leaves、deadline、nested epoll、child sequence、normal signal frame、exact reap を一つの graph へ統合する。
- 必須 backend semantics を完成させ、固定 quantum、sleep retry、samplingを削除する。終端は周期確認0件と全 userland build。

### Step 4 — graphics, keymap, session cutover

- sync-file と generic wlroots fence handoffを実装し、threaded llvmpipe、通常 memfd keymap、session supervisor、persistent seatd を有効にする。
- TTYから direct Sway、first frame、Foot PTYまで通す。終端はlauncherを編集せず、preloadと`LP_NUM_THREADS`がprocess tree/environmentに無いこと。

### Step 5 — stabilization and acceptance

- dead pathと一時instrumentationを削除し、最終observable contractだけの最小testを追加する。
- normal exit、SIGTERM、SIGKILL、10回lifecycleのresource差分を確認し、第12節を満たす。

subagent は各 Step 内で一回につき一つの短い patch を担当し、確認は原則 build までとする。
最大三つを並列化できるが、
shared header、ABI numbering、同じ source file、rootfs、QEMU は Root が直列管理する。
worker は broad investigation、全 backend 移行、QEMU battery を担当しない。

## 11. test policy

再設計中は途中構造を test で固定しない。必須確認は compile/link、ABI layout の static assert、
明白な bounds/ownership sanitizer 相当の host check だけとする。既存 test は診断材料として実行できるが、
green を維持するための adapter、旧挙動の snapshot、backend ごとの一時 regression は作らない。

direct Sway と Foot が新経路へ到達し、削除する経路が確定した後にだけ次の contract test を追加する。

- fd/OFD: dup と CLOEXEC、shared state、final close once
- lease/transfer: process death、mapping-after-close、multiple fd の A→B→C
- wait/signal: child exact reap、lost wakeup、nested epoll、deadline、service HUP
- graphics: sync-file ownershipと producer completion before consume

test は内部 struct layout や function call 列ではなく、最終的な observable contract だけを固定する。
既存 QEMU test は integration 開始まで gate にせず、前の ABI や launcher marker を要求するものは
最終経路の test へ置換する。worker は QEMU を実行せず、Root が一枠で直列実行する。

最終時だけ次を実行する。

1. TTY から direct Sway を起動し、output commit、page flip、screendump pixel を確認する。
2. Sway から Foot を起動し、PTY shell command の実行と exact child reap を確認する。
3. Sway 環境と process tree に launcher、preload、`LP_NUM_THREADS` が無いことを確認する。
4. threaded llvmpipe で連続 frame の未完了scanout、tear、stale frame が無いことを確認する。
5. normal exit、SIGTERM、SIGKILL の各 resource snapshot が同一 run baseline へ戻ることを確認する。
6. 上記 lifecycle を 10 回実行し、process/thread/LPR fd/native fd/service handle の差分 0 を確認する。

## 12. Phase 1 完了条件

- `/usr/bin/sway` と Foot が launcher なしで実用経路を通る。
- TTY、PTY、child、signal、wait に周期 poll と lost wakeup がない。
- Filed が LPR fd kind を解釈せず、fork/exec failure が元 process を変更しない。
- service resource は lease owner death で即時回収され、次回 open を待たない。
- SCM_RIGHTS の再転送と multiple-fd transaction が成立する。
- keymap は通常 memfd 経路、graphics は sync-file handoff を使う。
- threaded llvmpipe が有効で、`LP_NUM_THREADS=0`、preload、shader-cache 無効化がない。
- direct lifecycle 10 回後の resource 差分が 0 である。
- launcher source と既存 fixture は保全されているが、製品経路と新 test から参照されない。
