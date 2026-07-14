# Phase 6 LPR fd 操作・ライフサイクル統一設計

日付: 2026-07-14

状態: **Phase 6 実装基準 (2026-07-14 改訂)**。baseline は HEAD `71d0182` (M5.8R 完了、tree clean)。本書の Step と oracle は M5.6R `818c191`、M5.7R `d104007`、M5.8R `00e8014` 適用後のコードを正とする。

## 0. 目的と範囲

目的は、Linux-visible fd の操作、状態、native capability、backend handle、fork/exec/close、poll/epoll、SCM_RIGHTS を一つの拡張点へ集約することである。新しい fd 種を追加するとき、中央の kind 列挙を何箇所も更新するのではなく、原則として `lpr_fd_ops_t` 一個を定義すれば全操作へ参加できる構造を完成させる。

この Phase で最終的に達成するものは次の通り。

- 全 Linux-visible fd を entry → object → ops で解決し、raw native fd への fallback をなくす。
- `FD_CLOEXEC`、open-file-description status、backend handle、native wait capability の source of truth を一本化する。
- fork/exec/close と SCM_RIGHTS を、backend 共通の prepare → confirm/rollback transaction にする。
- M5.6 の filed memfd/VMO 転送を `filed` ops の `transfer` 実装として追加する。`VMO_FILE` のような横断的な特別 kind は作らない。
- 最後に実 wl_shm client の 196,608-byte buffer、中央 `#336699` screendump、KEY_A/REL `+7,-4`/BTN_LEFT 入力、Sway/seatd/client の normal/SIGTERM/SIGKILL lifecycle を新構造上で通す。

非目的は次の通り。

- この設計を理由に kernel に Linux fd table、LPR kind、filed/netd の path や設定知識を入れない。
- M5.6b の ext4 journal 修正、`koboxd`、`_kobox`、unlink journal smoke を fd-ops の都合で変更しない。ただし M5.6R 後に切り分けた Mesa shader-cache/ext4 data corruption は本設計と独立した Phase 6 序盤 leg として、fd-ops Step 1 より先に直す (§14)。
- SMP、Sway 高速化、fake PRIME fence 等の独立課題を fd-ops の Step 列へ混ぜない。libinput/XKB 入力は M5.7R/M5.8R で旧構造上の end-to-end を達成済みであり、本書では最終 Step の新構造上 regression oracle とする。Phase 再編は §13、送りメモ全項目の位置付けは §14 で定める。
- 旧 ABI を互換 shim で保持しない。ABI を変える Step は、理由を明示して版を上げ、同一 image の全 component を一括更新する。

撤退 patch は以下を本文中で `attempt1`、`attempt2` と呼ぶ。

- `attempt1`: `/tmp/claude-1000/-home-kamer-os/a050fa95-216d-41ab-80e4-d512d260f19a/scratchpad/m5.6-8-attempt1.patch`
- `attempt2`: `/tmp/claude-1000/-home-kamer-os/a050fa95-216d-41ab-80e4-d512d260f19a/scratchpad/m5.6-8-attempt2-full.patch`

61 files / +1862 と 88 files / +7923 は依頼時に示された機能差分の計測値として扱う。patch archive 全体には独立修正や M5.6b 差分も含まれるため、archive 全体の raw stat と混同しない。

### 0.1 改訂履歴

| 日付 | baseline | 内容 |
|---|---|---|
| 2026-07-13 | `f9a0e9f` 時点 | M5 を機能優先で完走するため執筆途中で凍結。 |
| 2026-07-14 | `71d0182` | M5.6R〜M5.8R 後の enum/dispatch/SCM_RIGHTS/exit/signal/lock/耐久実測を再棚卸しし、Step 1、owner-lease oracle、最終入力 oracle、Phase 番号、送りメモ対応を実装基準へ更新。既決の不変条件、v2/v7 shim なし、kernel 六点許可制は維持する。 |

## 1. 現状の正確な棚卸し

### 1.1 T3.3 でできたことと、まだ残るもの

T3.3 の entry/object 分離は既に入っている。entry は `active`、`fd_flags`、`file_index`、object は `kind`、`refcount`、`status_flags`、`rights`、`backend_id`、`offset`、payload を持つ (`userland/personality/linux/runtime/lpr_fd/table.h:173-191`)。`dup` は entry を増やして同じ object の refcount を上げる (`lpr_fd/table.c:347-405`)。したがって「shadow table が完全に別配列」という旧 `lpr-state-design.md` の棚卸しは現在の HEAD にはそのまま当てはまらない。

しかし object の共通 header と kind payload が同じ意味を二重に持ち、操作側は依然として kind cascade である。T3.3 は土台を作ったが、dispatch と backend lifetime は統一されていない。

さらに、object pointer を table lock 解放後にそのまま返す API がある (`lpr_fd/table.c:529-542`)。別 thread の close/reuse と競合できるため、ops 化では pointer getter を残さず、generation を検証する pinned reference に置き換える必要がある。

### 1.2 kind 数と分岐数の訂正

現 HEAD の runtime enum は `EMPTY` と、`FILED / DEVICE / TTY / DRM / INPUT / PIPE / EVENT / SOCKET / EPOLL / DMABUF` の実体 10 kind である (`lpr_fd/table.h:5-17`)。「11 kind」は `EMPTY` sentinel まで数えた値であり、実体 kind が 11 個あるわけではない。一方 exec wire には runtime object のない `NATIVE` tag も残る (`userland/filed/include/filed/payload.h:57-66`)。これは native fallback が正式 object になっていないことの別の表れである。

再現可能な current-HEAD 計測は次の通り。

- `rg -n 'LPR_FD_TABLE_KIND_' userland/personality/linux/runtime --glob '*.[ch]'` は **165 reference lines / 12 files**。
- 内訳は `control.c` 63、`table.h` 22、`lpr_socket.c` 21、`lpr_epoll.c` 21、`exec.c` 15、`table.c` 11、`dup_pipe.c` 6、`lpr_drm/client.c` 2、`lpr_vfs/ops.c` / `lpr_tty/client.c` / `lpr_timerfd.c` / `lpr_input/client.c` が各1行である。
- `rg -n 'kind[[:space:]]*(==|!=)[[:space:]]*LPR_FD_TABLE_KIND_|switch \(.*kind.*\)|case LPR_FD_TABLE_KIND_' userland/personality/linux/runtime --glob '*.[ch]'` の direct syntactic scan は **120 lines**。複数行比較の enum 行を数えず、一つの switch の各 case は別々に数えるため、意味上の「分岐箇所数」ではない。

凍結時の同じ direct enum scan は 149 lines / 12 files (`control.c` 63、`table.h` 22、`epoll.c` 16、`lpr_socket.c` 10) だった。M5.6R〜M5.8R で socket が 10→21、epoll が 16→21 となり、総数は16 lines増えた。これは機能優先実装が旧 chain を太らせるという旧 §13 の予測どおりである。なお accessor (`lpr_linux_*_fd_active`) を使う `io.c` や `metadata.c` の cascade は direct enum scan に入らないので、165 を「全分岐数」とは呼ばない。負債の実体は次の concrete chain で確認する。

| 操作 | 現在の分岐 |
|---|---|
| read | device → input → DRM → tty → timerfd → eventfd → pipe → filed → native fallback (`lpr_vfs/io.c:157-258`) |
| close | device/epoll/socket/tty/DRM/input/dmabuf/event/pipe/native/filed を再列挙 (`lpr_fd/metadata.c:306-381`) |
| close_range | CLOEXEC 用の kind chain と native table 用の別 loop (`lpr_fd/metadata.c:384-467`) |
| fcntl | kind ごとに同じ `GETFD/SETFD/GETFL/SETFL/DUPFD` switch を複製 (`lpr_fd/metadata.c:562-817`) |
| ioctl / fstat | ioctl は input/DRM/tty chain (`lpr_fd/metadata.c:835-853`)、fstat は device/event/input/dmabuf/DRM/tty/pipe/native/filed chain (`lpr_fd/metadata.c:897-1005`) |
| syscall dispatch / mmap | socketだけをread/write/close/ioctl/readv/writev/fstat/fcntlの入口で分岐し (`lpr_dispatch.c:900-944,974,1099`)、mmapはdmabuf/DRM/FILEDを再列挙する (`lpr_dispatch.c:568-588`) |
| dup/dup2 | source kind chain (`lpr_fd/dup_pipe.c:198-397`) と source/target validity・close chain (`lpr_fd/dup_pipe.c:404-475`) |
| poll | event/tty/DRM/input/pipe/filed/device/native/socket readiness chain (`lpr_socket.c:1762-1865`) と native wait source chain (`lpr_socket.c:1882-1950`) |
| epoll | 登録可能 kind の allowlist (`lpr_epoll.c:276-283`)、nested scan (`lpr_epoll.c:414-465`)、native wait fd 選択 chain (`lpr_epoll.c:479-527`) |
| exec | serialize の else-if chain (`lpr_process/exec.c:248-307`)、self-exec close (`lpr_process/exec.c:450-517`)、socket別 cleanup + process exit chain (`lpr_socket.c:640-655`; `lpr_process/exec.c:519-566`) |
| restore | kind 別 restore と最後の switch (`lpr_fd/control.c:683-971`) |

M5.3c の cube fd 化けは、厳密には「kind switch の更新漏れ」が根因ではなかった。drmd open 後の補助 uevent publication が `ENOENT` になり、その値を special-path の「この service の path ではない」と解釈して filed open へ fall through したものだった (`pacha_docs/refactor-plan.md:535-537`; 現在の fallback は `lpr_vfs/ops.c:546-566`)。したがってこれは kind 追加漏れの直接実績とは数えない。ただし、未知・失敗を filed/native へ暗黙 fallback すると「別 kind として成功する」という同じ failure class の実績であり、目標構造では unsupported/unknown を必ず fail closed にする。

### 1.3 共通状態の二重帳簿

| 概念 | 現在の重複 | 根拠 |
|---|---|---|
| backend handle | object `backend_id` と filed/tty/DRM/input/socket payload の `handle`、dmabuf の `token` | `lpr_fd/table.h:24-32,61-104,106-135,180-190`; install 時に両方へ書く `lpr_fd/control.c:269-323` |
| native wait fd | DRM/input/socket payload の三複製 | `lpr_fd/table.h:77-95,106-135` |
| Linux flags | entry `fd_flags`、object `status_flags`、各 payload の `flags` | `lpr_fd/table.h:24-144,173-190`; payload へ再同期する switch `lpr_fd/control.c:384-442` |
| socket CLOEXEC | entry `fd_flags` のほか payload byte `cloexec` | payload 定義 `lpr_fd/table.h:106-114`; writes は `lpr_fd/table.c:178-179`, `lpr_fd/control.c:316-317,931`, `lpr_socket.c:596,954,1599`; read は `lpr_socket.c:437-440` |
| filed offset | object `offset` と filed payload `offset` | `lpr_fd/table.h:24-32,180-190`; setter が両方へ書く `lpr_fd/control.c:520-548` |

`FD_CLOEXEC` と `O_NONBLOCK/O_APPEND` は同じ flags の複製ではなく、Linux 上も前者が descriptor entry、後者が open file description という別概念である。問題は、その正しい二分に加えて payload `flags` と socket `cloexec` が mirror になっている点である。目標では entry/object の意味上必要な二分は残し、mirror だけを全廃する。

また T3.3 で決めた通り、filed offset の正は filed の open-file-description とする (`pacha_docs/lpr-state-design.md:298-302`)。現在の LPR writable shadow は fork/SCM_RIGHTS をまたいだ共有 offset の正にはできない。

### 1.4 Linux fd と native fd の名前空間衝突

kernel native fd table は 256 entries、dynamic allocation の開始は 16 である (`kernel/abi/fd_abi.zig:1-2`)。一方 Linux-visible table は初期 256、最大 `INT32_MAX + 1` を想定する (`lpr_filed_internal.h:28-29`)。LPR の fixed native endpoints は 240〜246 にある (`lpr_filed.h:6-7`, `lpr_socket.h:6`, `personality/lpr_image_abi.h:83-87`)。

ただし 16 は default allocationとsyscall result encodingの境界であり、既知fdの型/validity境界ではない。kernel内部のindex/allocatorは0〜255の任意の`min_fd`を受け付け (`kernel/src/state/fd.zig:149-159`)、`FD_DUP`もcaller指定の低い`min_fd`へinstallできる (`kernel/src/state/fd.zig:1074-1088`)。一方 syscall statusは0〜6を使う (`kernel/src/syscall/numbers.zig:88-95`) ため、public native wrapperはallocation/dup/receiveをnamed minimum 16以上に制約し、既知fdのvalidityは`FD_GET_INFO`で判定する。`fd < 16`を任意の既知整数へ当てて「nativeではない」と判定してはならない。

現在の logical allocator は、entry が空でも同じ整数に native fd が存在すれば使用不可とする (`lpr_fd/control.c:632-664`)。pipe と dmabuf は logical fd 番号をそのまま kernel syscall に渡し (`lpr_vfs/io.c:193-218`, `lpr_fd/dup_pipe.c:244-285`)、exec v7 は pipe の `handle=fd` と raw `native_wait_fd` を直列化する (`lpr_process/exec.c:277-305`)。これは「二つの表が同じ整数を偶然共有する」設計であり、一方の allocation/reuse が他方へ漏れる。

`rg -n '(<|>=)[[:space:]]*16|16[[:space:]]*(<|<=)'` を `lpr_socket.c` と `lpr_process/exec.c` に掛けた current scan では、fd-like variable と16を比較する行が **44 lines** ある。すべてが衝突判定ではなく、native syscall result の success/error decode に使う箇所もあるが、同じ literal が result decode、validity、ownership、serialization sentinel を兼ねること自体が問題である。libpacha にも `result >= 16` を直接変換する一箇所がある (`userland/libpacha/src/syscall.c:79-85`)。

attempt2 の retransfer fixture は、受信 logical fd が意図的に 3〜15 であることを要求し、その fd をさらに SCM_RIGHTS で転送する (`attempt2:7241-7302`)。これは「Linux fd が 16 未満なら native capability でない」という判定を契約に使えないことを実測で固定した。低い logical fd の再転送不能は test を弱めて回避せず、名前空間分離で消す。

native syscallのreturn domainにも別の衝突がある。statusは`OK=0, error=1..6`だが (`kernel/src/syscall/numbers.zig:88-94`)、`FD_READ/WRITE/READV/WRITEV`は同じreturnに0以上のbyte countも返し、複数error pathは正のstatusを返す (`kernel/src/syscall/fd.zig:131-234,252-367`)。従って成功した1〜6 bytesとerror 1〜6をuserland decoderだけでは区別できない。`FD_POLL/FD_WAIT_MANY`もready count 1〜6とerrorが重なり (`kernel/src/syscall/fd.zig:472-515`)、current libipcは`revents`を再scanして一部を推測する (`userland/libipc/src/ipc.c:152-179`)。`FD_FCNTL(GET_FLAGS)`とrequestによってvalueを返す`FD_IOCTL`にも同じ問題がある (`kernel/src/syscall/fd.zig:370-382,940-979`)。pipe errorだけは既に負statusへ変換する (`kernel/src/syscall/fd.zig:100-107`) ため、ABI自身も一貫していない。typed namespace APIを成立させる前提として、Step 3でcount/value-return syscallのerrorだけを`-1..-6`へ正規化する必要がある。

### 1.5 fork / exec / close の非統一

- table primitive の final close は refcount を減らして object を zero clear するだけで backend callback を持たない (`lpr_fd/table.c:319-332`)。そのため実 backend close は `metadata.c` と socket code に再実装される。
- filed/tty/DRM/input/dmabuf は kernel clone 後の child で個別に duplicate する (`lpr_fd/dup_pipe.c:17-84`)。途中失敗は記録するだけで fork 自体を失敗にできず、rollback もなく、dmabuf は失敗時に object を破壊する。M5.6R は AF_UNIX socket だけ clone 前に `NETD_OP_DUP` を予約する別経路を足したが、table lock 中にRPCし、callerがcancelを手書きする (`lpr_socket.c:658-695`; 呼出し・cancelは `lpr_process/syscalls.c:382-519`)。event/epoll はいずれのbackend transactionにも載らない。
- exec serialize は 9 kind の else-if chain で、EPOLL は CLOEXEC でなくても preserve 対象から除外される (`lpr_process/exec.c:199-215,248-307`)。
- M5.6R は non-CLOEXEC AF_UNIX socket を exec 前に `NETD_OP_DUP` 予約し、self-exec close の `NETD_OP_CLOSE` から target handleを守る ad-hoc pathを追加した (`lpr_socket.c:698-735`; `lpr_process/syscalls.c:975-1031`)。従って旧ドラフトが指摘した inherited socket 即EBADFは現経路では抑止済みだが、socketだけが独自reserve/release順序を持つためStep 19のgeneric exec transactionで削除する。
- exec commit は `PROCESS_EXEC_FROM` より先に `lpr_close_local_state_before_self_exec()` を実行し、kernel commit が失敗しても旧 table/backend ref を復元しない (`lpr_process/syscalls.c:1024-1037`)。新 image 側 restore も entry を先頭から逐次 publishし、途中失敗を巻き戻さないうえ、install 完了前に global installed flag を立てる (`lpr_fd/control.c:973-992`)。
- v7 は object identity を持たないため、同一 object を指す dup aliases も entry ごとに serialize される (`lpr_process/exec.c:248-307`)。restore は FILED を entry ごとに新 object へ入れ、TTY だけ raw handle 一致を heuristic に alias 化する (`lpr_fd/control.c:683-739`)。shared offset/status/refcount を全 backendで保存する契約になっていない。
- M5.8R は `lpr_linux_prepare_process_exit()` の先頭から socket専用loopを呼ぶようにした (`lpr_process/exec.c:519-523`; `lpr_socket.c:640-655`)。後続loopは依然 FILED/DRM/INPUT/DMABUF の列挙で (`lpr_process/exec.c:524-566`)、TTYはnormal exit cleanup対象外である。socketを含むこの手動per-kind path全体をStep 10のunique-object generic close-allが削除する。SIGKILLではuserland loop自体が走らない。

attempt2 は filed owner fork と socket ref reservation を別々に追加し、caller が両方の cancel path を手書きしていた (`attempt2:11204-11346,12199-12295,12676-12732`)。clone 前予約という方向は正しいが、backend を増やすたび top-level cancel sequence が増えるため、一般化されていない。

### 1.6 poll / epoll / fd_wait_many

current poll は kind ごとに readiness を scan し (`lpr_socket.c:1762-1865`)、pipe/DRM/input/socket だけを別 chain で native wait fd へ変換する (`lpr_socket.c:1882-1889`)。集合の全要素がこの native source chain に載る場合だけ `fd_wait_many` を使い、一個でも載らない fd が混じると集合全体が10ms sleep/recheckへ落ちる (`lpr_socket.c:1891-1971`)。

epoll も同じ kind allowlist と wait-fd chain を別実装する (`lpr_epoll.c:276-283,479-527`)。M5.7R の nested epoll はlevel-readable再帰scanを追加したが、EPOLL object自身はnative wait sourceを持たないため、non-native targetを一つ含むと `all_native` がfalseとなり10ms quantumを使う (`lpr_epoll.c:414-465,479-510`)。close時にはEPOLL kindだけを特別unmapする (`lpr_epoll.c:623-675`)。新kindがpoll実装を持っても、epoll allowlistとblock pathの両方を更新しなければ参加できない。このnested 10ms pathはStep 11のplannerを前提に、Step 12でEPOLL object/interestを同plannerへ載せた時点で削除する。Step 11だけではまだ消えない。

kernel の `fd_wait_many` 自体は既に存在する (`kernel/abi/fd_abi.zig:15`, dispatch は `kernel/src/syscall/fd.zig:1011-1014`)。不足しているのは kernel 機能ではなく、各 object が「今 ready か」「何を native wait set に arm するか」「wake 後にどう recheck するか」を共通形式で返す LPR 側の境界である。

### 1.7 現 SCM_RIGHTS の範囲と破綻点

M5.6R の current AF_UNIX sendmsg/recvmsg は INPUT/DRM に加えて FILED memfd を一個だけ明示許可する。LPR は FILED handleをdupして `NETD_FD_KIND_FILED_MEMFD=0x100` に変換し、receiverで FILED payloadを直接組み立てる (`lpr_socket.c:1137-1315`)。netd wireには `fd_kind/fd_flags/fd_handle/fd_aux` が露出し、header自身もこの第三kindを「M5.6R one-way」と定義する (`userland/netd/include/netd/ipc_protocol.h:44-45,95-104`)。これは本節が「採用しない構造」としたkind-aware netdを、M5機能優先の暫定経路として実装した事実であり、目標不変条件を変更する根拠ではない。Step 21でINPUT/DRMだけでなくFILED_MEMFDもopaque brokerへ移し、このwire kindを削除する。

netd は ancillary を一個だけ socket stateに保存する。M5.6Rで先頭byteの相対`fd_offset`を持ったため、anchorより前だけを読むrecvはfdを消費せずoffsetを繰り下げるが、pending ancillary中の次batchは拒否され、複数occurrence/alias/batch境界を表現できない (`userland/netd/src/unix_socket.c:168-227`)。socket破棄時にFILED_MEMFD handleとnative wait fdはnetdが回収する一方、INPUT/DRM service handleのdiscardはLPRのkind別pathに残る (`unix_socket.c:239-253`; `lpr_socket.c:454-460,1259-1312`)。queue/socket破棄時のbackend ref ownershipがnetdとLPRへ分裂し、全kindを一律回収する契約がない。

M5.8R の `MSG_PEEK` はnetdでdataをcopyしてcursor/ancillary原本を残すだけで、peek呼出しへ新しいfdを複製しない (`userland/netd/src/unix_socket.c:192-206`)。LPRの`FIONREAD`は `PEEK|DONTWAIT` のNETD recvをbyte-count probeに使う (`lpr_socket.c:1614-1653`)。従って §6.3 の「repeated PEEKごとに独立receiver entry/OFD ref、CLOEXEC、capacity rollback」を満たさず、現FIONREAD greenはancillary PEEK semanticsの証明ではない。この距離はStep 21のred testで閉じる。

M5.8R の `reap_orphaned_sockets()` は新規socket open時、notify channelをHANGUP pollしてsocketを強制破棄する (`userland/netd/src/unix_socket.c:56-85`)。これはowner leaseのad-hocな前身であり、Step 16でside-aware lease closeへ置換する。判断根拠はnotify peerのcapability closeが作るHANGUPであってPIDや`notify_pending`ではないため、「`notify_pending`をalive oracleにしない」原則とは整合する。ただしcurrent kernel HANGUPはside別refを数えず総`ref_count==1`を見るだけで (`kernel/src/state/fd.zig:687-692`)、normal close/direct recv/execを含む完全なlease契約ではない。

一方、kernel IPC の capability transfer は既に source の `TRANSFER` right と requested-rights subset を検査し、message queue 用の object ref を retainし、enqueue 成功後だけ `MOVE` source を closeする (`kernel/src/state/ipc.zig:743-820`)。message/queue破棄も同梱 capabilityを releaseする (`kernel/src/state/ipc.zig:154-187`)。従って generic SCM_RIGHTS のための新 syscall は不要であり、LPR/service側に欠ける OFD ticket、opaque framing、stream ordering、owner leaseをこの既存 capability ownership上へ構成する。

filed には再利用できる土台もある。`filed_open_file` が offset/status/rights/refcount、`filed_handle` が descriptor handleを持つ (`userland/filed/include/filed/vfs.h:114-136`)。dup handle は同じ open-file targetを参照し refcountを上げる (`filed/src/vfs/core.c:782-837`)。memfd転送はこの OFD identityを export/importし、VMOだけを別物として運ばない。ただし `filed_handle.fd_flags` はLPR entryとの mirrorなので、新契約では canonical sourceにしない。

attempt1/attempt2 から採用する事実と、採用しない構造を分ける。

採用する事実・oracle:

- SCM_RIGHTS は sender close 後も queue が独立参照を持ち、受信後は receiver が持つ。
- stream ancillary は `sendmsg` 先頭 byte の absolute rx offset に結び付け、その byte を含む recvmsg だけへ渡す (`attempt1:553-557`)。
- 196,608-byte memfd の先頭/末尾、MAP_SHARED writeback、per-recvmsg CLOEXEC、shared offset/status/seals、3〜15 の fd を使う二 hop retransfer を検査する (`attempt2:6722-6751,6915-6974,7180-7302,7578-7670`)。
- sender/receiver/queued owner の SIGKILL、filed transfer capacity、netd 32 endpoint 再確保を同一 VM で検査する。単に object count を見るだけでは不十分である (`attempt1:555-557`; `attempt2:7610-7670`)。
- fork/exec の前に backend ref を予約すること、service owner lease を capability peer-close へ結び付けることは必要である。

採用しない構造:

- `VMO_FILE` を FILED の直後へ挿入し、read/fcntl/fstat/mmap/dup/exec/restore 全 chain を増やす方式。attempt1 は ABI 番号を正しく全てずらした点はよいが (`attempt1:551-553`)、まさに今回除去する拡散を再現した。
- netd が `NETD_TRANSFER_INPUT / DRM / FILED_TOKEN` を理解する方式 (`attempt2:9127-9129,12478-13119`)。broker は capsule を opaque に保持し、kind 解釈は LPR ops と所有 service に戻す。
- filed owner fork、socket fork、exec socket reserve を top-level が別順序で呼ぶ方式 (`attempt2:11085-11346,12199-12295`)。
- raw `<16` relocation helper を各機能に増やす方式。低 logical fd test を満たしても、名前空間の二重性を温存する。
- v7 descriptor に `ofd_id` や追加 field を継ぎ足し、filed が引き続き全 kind を validate する方式 (`attempt2:4158-4222,11969-12137`)。

両patchを「再利用するcode」ではなくcontract test/defect evidenceとして整理すると次になる。

| 実測済みの欠陥/契約 | patch根拠 | 本設計で固定する場所 |
|---|---|---|
| fork後の親closeでchild共有handleを壊す | clone前socket予約へ変えた経緯 (`attempt1:4036-4102,555`) | unique object fork transaction、Step 18 |
| execでpreserve socketのsource最終refを先にcloseしEBADF | socket exec ref予約を別実装した箇所 (`attempt1:4062-4091`; `attempt2:12030-12060`) | target materialize後だけsource release、Step 19/20 |
| `notify_pending`/HUP queueをaliveと誤認し32 endpointsが枯渇 | 20 sender-killで再現した記録 (`attempt1:555-557`) | side owner leaseとcapacity oracle、Step 14/16/21〜23 |
| filed RPCとnetd page/queue lockのlock order逆転 | RPCをwire page取得前へ出した制約 (`attempt2:13220-13231`) | table/queue unlock下のops transaction、Step 9/21 |
| logical 3〜15へ受信したVMOをnative fd扱いできず再転送不能 | low-fd two-hop oracle (`attempt2:812-823,7221-7304`) | typed namespace + hidden native ref、Step 3/5/23 |
| fd flagsとOFD flags/offset/sealsの共有単位が混同される | per-recvmsg CLOEXEC、shared OFD/readonly/seals oracle (`attempt2:812-823,6978-7218`) | entry/object分離、unique capsule/manifest、Step 2/20/23 |
| per-service reserve/cancelをcallerへ並べるとerror pathが増殖する | socket/filed ownerを別順序でcancel (`attempt2:12199-12237`) | generic transaction ledger、Step 14〜18 |
| table lock中にbackend RPCする | socket fork reserveがlock中にnetd DUP (`attempt2:12676-12699`) | pin/snapshot → unlock → prepare → revalidate、Step 9/18 |

attempt2の`ofd_id`は「alias identityをwireに出す」着眼は採用するが、entryごとの追加fieldと過去entry scanは採用しない。v8/transfer batchはunique object recordを一回書き、entry/occurrenceがid参照するためlinearに検証できる。

### 1.8 signal / thread-exit の M5 到達点と残るred

M5.6R で async entry の call 直前 alignment は修正済みである (`lpr_entry.S:75-87`)。poll/epoll もnative blockingから復帰した直後にpending frameをdeliverする (`lpr_socket.c:1875-1879,1921-1935`; `lpr_epoll.c:572-575`)。kernel schedulerは`pending_signal`とhot stateをlock下でpublishした後にwaiterをwakeする (`kernel/src/scheduler_connection.zig:2260-2277`)。これらはStep 1の新規作業ではなく、維持すべきM5.6R regression baselineである。一方restorerにはcall前の余分な`sub $8,%rsp`が残る (`lpr_entry.S:89-98`)ため、同じSysV alignment testでこちらだけがredにする。

M5.8R はprocess-wide signalが「最初のblocked thread」へ配送される現契約も固定した (`kernel/src/scheduler_connection.zig:2255-2288`)。Swayではevent-loop以外のblocking threadがSIGTERMを受けるとmain-loop teardownが進まず、10秒後のSIGKILLへ必ずescalateする (`lpr_sway_launcher.c:275-311`; runner oracleは `tests/run-lpr-qemu-sway-endurance-smoke.sh:34-73`; 切り分けは `refactor-plan.md:614`)。Step 1でprocess-directed signalのowner threadを明示登録し、schedulerがそのthreadだけをwake/deliverする契約に変える。これはthread選択を行うkernelでしか完結しないため、独立redと事前許可の対象である。

active-stack thread exitも「再現した場合だけ」の候補ではない。M5.8Rでmusl `__unmapself`が実行中stackを先にunmapするredは再現済みである (`refactor-plan.md:612`)。currentはmusl固有の8-byte tailを照合し (`lpr_dispatch.c:919-927`)、静的4KiB stackとinline asmでmunmap→thread-exitする (`lpr_process/syscalls.c:20-26,170-210`)。musl更新で命令列が変われば壊れる暫定対処なので、Step 1でmusl byte signatureに依存しないredを固定し、許可後にkernel `thread_exit` post-switch-unmap flagへ置換する。

### 1.9 M5.8R 耐久の一次red data

current endurance smokeは残存leakを「解決済み」とは扱わず、強制終了回の増分を決定的に固定する。

- filedは強制終了1回ごとにちょうど`+4 handles`で、シグネチャは`.memfd-15`, `.memfd-17`, `wlroots-AAAAAA`, `wayland-1.lock`である (`tests/run-lpr-qemu-sway-endurance-smoke.sh:86-108`; `refactor-plan.md:610,622`)。Step 15完了時に`+4`許容を削除し、normal/TERM/SIGKILLの各回でfiled handles/sessionがbaselineへ戻るoracleへ反転する。
- drmdは強制終了1回ごとにownerless handles `+5`、その内訳にFB `+2`、dumb `+4`を含み、6回で`DRMD_HANDLE_MAX=32`を圧迫する (`refactor-plan.md:614,622`; max定義は `userland/drmd/src/drm_island.c:20`)。そのためcurrent fixtureはTERMを2/7、direct KILLを4/9の計4回にcapしている (`tests/fixtures/sway_endurance.sh:27-35`)。Step 16完了時にこのcapを外し、direct SIGKILL 20回連続の毎回filed/drmd/netd全handleをbaselineへ戻す。
- SIGTERMは`kind=exit status=137 escalated=1`、direct SIGKILLは`kind=exit status=137 escalated=0`をcurrent runnerが厳密に要求する (`tests/run-lpr-qemu-sway-endurance-smoke.sh:34-50`; launcherの10秒wait/escalationは `lpr_sway_launcher.c:293-323`)。Step 1完了時にTERMを`kind=exit status=0 escalated=0`のgraceful teardownへ反転する。direct SIGKILLは`137/escalated=0`のままである。
- M5.8Rはpeer exitがthread countを2→1にした時のstale fd-table lock wordも再現し、unlockをunconditional exchange-clearにした (`lpr_fd/table.c:53-66`; unit red/greenは `tests/lpr_fd_table_test.c:163-183`; `refactor-plan.md:612`)。これは後続のpin/lock設計のcurrent baselineであり、naked pointerやtable lock中RPCまで解決したわけではない。

Step 23はこのservice-wide owner oracleにSCM queue/sender/receiverの各20回を追加し、Step 24は20回SIGKILLを含む最終Sway/input回帰として再度固定する。従って後段のStepが前段の反転を緩めることはない。

## 2. 目標不変条件

実装完了時には次を invariant とする。

1. Linux syscall が受け取る整数は `lpr_linux_fd_t` であり、必ず fd table entry を引く。entry がなければ `EBADF/POLLNVAL` であり、同じ整数の native fd を探さない。
2. object type の dispatch は `object->ops` だけで行う。中央 code は kind enum を switch しない。unknown wire kind、NULL callback、unsupported operation は明示エラーであり filed/native へ fall through しない。
3. `FD_CLOEXEC` は entry の一箇所だけ、access mode と status は open-file-description common state の一箇所だけ、backend handle と native wait capability は object common header の一箇所だけにある。kind payload に同名 mirror を置かない。
4. `dup/dup2/F_DUPFD` は entry alias を増やし、同一 process 内では backend DUP RPC や native fd dup を行わない。backend/native resource は object の final ref だけが解放する。
5. fork/exec/close/transfer は unique object ごとの transaction ticket を使う。prepare 後の全失敗 path は reverse-order rollback、commit/rollback は idempotent、owner death は lease close で回収できる。
6. fd table lock を保持したまま ops callback、service RPC、`fd_wait_many` を呼ばない。snapshot/pin → unlock → external work → generation revalidate/commit の順にする。
7. exec/transfer wire は raw native fd number、runtime pointer、内部 file index を永続表現にしない。native capability は ordinal と実 capability transfer で運ぶ。
8. netd は ancillary capsule の kind を解釈せず、byte ordering、queue ownership、delivery transaction だけを持つ。filed RPC を netd socket/page lock 下で呼ばない。
9. process normal exit と SIGKILL の双方で同じ backend resource が最終的に解放される。normal exit callback は早期回収、lease は crash cleanup の正である。
10. table/queue/kernel execのvisible commit後、`CONFIRM`は失敗可能な新規allocationを行わない。transport errorは同じtxn idのdurable retryになり、visible stateをrollbackしない。

## 3. 共通 data model と名前空間

### 3.1 entry と object

概念構造は次とする。これは layout の確定値ではない。

```c
typedef struct lpr_fd_entry {
    uint32_t object_index;
    uint32_t generation;
    uint16_t fd_flags;       /* FD_CLOEXEC only */
    uint16_t state;          /* FREE / RESERVED / OPEN */
    lpr_rights_t effective_rights; /* this descriptor's authority */
} lpr_fd_entry_t;

typedef struct lpr_fd_object {
    const lpr_fd_ops_t *ops;
    uint32_t refcount;       /* entry references */
    uint32_t pin_count;      /* in-flight syscall/epoll/transaction */
    uint32_t weak_watch_count; /* epoll watch shells; not an OFD root */
    uint64_t generation;
    uint32_t state;          /* OPEN / PREPARED / WATCH_ONLY / CLOSING / DEAD */
    lpr_ofd_key_t ofd_key;   /* stable, capability-bound identity */
    uint32_t access_mode;    /* O_RDONLY/O_WRONLY/O_RDWR, immutable */
    lpr_ofd_status_t status; /* canonical O_NONBLOCK/O_APPEND storage */
    lpr_rights_t rights_ceiling; /* proven backend/native authority */
    lpr_backend_ref_t backend;
    lpr_native_ref_t primary_native;
    lpr_native_ref_t wait_native; /* empty or alias-safe owned ref */
    void *kind_state;
} lpr_fd_object_t;
```

`cloexec` を object へ昇格しない。`dup(fd)` は新 entry の CLOEXEC を clear し、`F_DUPFD_CLOEXEC` は set するため、object に置くと Linux 意味論を表せない。object 共通領域へ昇格するのは backend handle、access/status、rights ceiling、native capability である。

rightsもCLOEXECと同様にdescriptorごとにattenuateされ得るため、operation可否に使う`effective_rights`はentryへ置く。`dup`はsource entryのrightsをcopyし、execはentryごとに保存し、transferは各occurrenceのsource entry rightsと実capability/backend policyの積集合をreceiver entryへ設定する。objectの`rights_ceiling`はLPRが実際に保持し検証済みのbackend/native authority上限であり、wire metadataをunionしてはならない。同じ`ofd_key`を弱いrightsで再importしても既存entryを弱化せず、強いrightsで再importした場合もそのentry以外を強化しない。必要ならproof済みのstronger backing refでceilingだけをupgradeできるが、全syscallは必ずentry rightsを先に検査する。

`ofd_key`は`{backend domain, stable id, generation}`の概念値と、その値をbackend/native capabilityへ結び付けるproofを持つ。wireから来た整数を信用せず、filed/netd等のowner serviceまたはnative transfer anchorがimport時に検証して返す。同じsource OFDを別々のsendmsg、fork、exec経由で同一processへ持ち込んでも同じkeyになり、process内canonical mapが既存objectへentryをaliasする。重複してprepareしたtarget attachmentはbackendのidempotent merge/releaseで一個へ畳む。stable key/proofを提供できないbackendはtransferをenableしない。

これは一sendmsg内だけの`batch_object_id`より強い契約である。`batch_object_id`はframing用、`ofd_key`は別transaction間も続くopen-file-description identity用である。status/offset共有だけでなく、epoll registration identityと「最後のentry close」判定も後者を使う。

bare native capabilityだけをidentity proofにはしない。現`FD_GET_INFO`が返すのはkind/rights/flags/size/extraだけで、kernel object id/generationを公開しない (`kernel/src/state/fd.zig:447-491`; `kernel/abi/fd_abi.zig:93-98`) ため、別々にtransferされた二capabilityが同じpipe endpoint/OFDかをLPRだけでは比較できない。final構造では各full LPR processが一つのshared OFD-state arena VMOを持ち、local/native objectは生成時に`{arena UUID, slot, generation}`を割り当てる。これが最初から`ofd_key`であり、後のfork/SCMでrekeyしない。statusやEPOLL graph等の共有stateも同slot/arena内またはそこから参照する。

最初のcross-process `PREPARE`でLPRSはidentity/state arena VMOとslot generationをgeneric OFD-anchor tableへ登録し、strong attachment/queue ticket countをowner lease下で保持してimport ticketを発行する。実pipe/data capabilityは各logical objectまたはqueueだけが保持し、LPRSが余分なreader/writer refをretainしてEOF/EPIPEを妨げてはならない。receiverはraw idでなく、trusted LPRがdata capabilityと一体に作ったLPRS ticketを検証して同じ`ofd_key`へcanonicalizeする。Linux guestはopaque capsule/ticketを構築できない。これはuserland ownership serviceであり、kernelへstable koid ABIを追加しない。arena/anchor recordのexact layout/capacityは未決だが、このproof経路、creation-time stable key、「raw `FD_GET_INFO`比較は禁止」は確定事項である。

process-wide arena capabilityをunrelated SCM receiverへ渡してはならない。一slotのfdを受け取ったprocessが別slotのstatus/EPOLL graphをread/writeでき、entry rightsを迂回するためである。同一fork lineage/exec targetはarenaを共有できるが、unrelated targetはLPRSのslot-scoped opaque ticket/RPCだけでそのslotを操作する。将来range-confined capabilityが証明できた場合だけslot mappingへ置換できる。export transactionはdata cap ordinalとslot ticketを同じtxnへ結び、LPRSはidentity/generation/shared state/countを保持するがdata cap自体は保持しない。

`lpr_ofd_status_t` は値そのもの、または backend が所有する共有 OFD state への typed reference のどちらか一つを表す。fork/SCM_RIGHTS 後にも `F_SETFL` が同じ open file description へ効くことが契約であり、LPR value と service value を同時に正としてはならない。service-backed object は backend OFD を正とし、local/native object はcreation-time arena slotを正とする。process-local common API は Step 2、arena/key allocationはStep 5、cross-process protocol境界はStep 13までにfault-injection testとともに確定するが、「二つを同期する」案は採用しない。

fork後にも共有されるmutable open-file-description stateは、native kernel objectが既に正ならそのcapability、そうでなければobject生成時のarena slot/opaque shared-state + mutation-notifyを最初から正にする。特にEPOLL interest graphはfork後に親子でADD/MOD/DELとwakeを共有するため、process heapの`kind_state`をcopyしてはならない。既存objectの段階移行はStep 17 commit前に一回だけarena stateへ移すが、final constructorにlazy promotion/rekey pathを残さない。LPRSはblobを解釈せずidentity/lifetimeだけを持ち、EPOLL opsがstable `ofd_key` edge、process-shared lock、generationを管理する。exact VMO layout/lock recoveryはStep 17開始前のred testで固定する未決事項である。

backend-private state は `kind_state` に残せる。例えば socket domain/type/protocol、input device index、event counter subtype は共通 flags/handle ではない。ただし generic な `active`、`flags`、`cloexec`、`handle`、`native_wait_fd` という mirror field は禁止する。

filed offset は filed backend OFD のみが所有する。LPR page-cache 最適化に observed offset を残す場合も、generation 付きで捨てられる cache とし、exec/transfer へ serialize せず、correctness 判定に使わない。cache を残すかは実測後の未決事項であり、最初の移行では削除を優先する。

### 3.2 logical/native namespace API

型と API を次の境界で分ける。

```c
typedef uint32_t lpr_linux_fd_t;       /* table index; kernel へ渡せない */
typedef struct { int raw; } lpr_native_fd_t; /* native module だけ unwrap 可 */
typedef struct lpr_native_ref lpr_native_ref_t; /* ownership + rights + generation */

typedef enum lpr_native_policy {
    LPR_NATIVE_OBJECT_OWNED,
    LPR_NATIVE_RUNTIME_DROP_CHILD,
    LPR_NATIVE_RUNTIME_RECREATE_CHILD,
    LPR_NATIVE_EXEC_MANIFEST,
    LPR_NATIVE_FIXED_ENDPOINT,
} lpr_native_policy_t;

lpr_native_fd_result_t lpr_native_decode_new_fd(long raw);
int lpr_native_decode_status(long raw);
int64_t lpr_native_decode_io_count(long raw, lpr_native_io_abi_t abi);
int64_t lpr_native_decode_value(long raw, lpr_native_value_abi_t abi);
int lpr_native_adopt(lpr_native_fd_t decoded, lpr_native_expect_t expect,
                     lpr_native_ref_t *out);
int lpr_native_dup_ref(const lpr_native_ref_t *, lpr_native_ref_t *out);
void lpr_native_close_ref(lpr_native_ref_t *);
int lpr_native_get_info(const lpr_native_ref_t *, pacha_fd_info_t *out);
```

`16` と `256` は libpacha/native namespace module の named ABI constants だけに置く。LPR と各 service は raw comparison で fd validity を判定せず、return-domain別のresult decode/adopt/get-info helper を使う。status/new-fd/I/O countは同じraw整数でも有効域が異なるため、一個の`decode_result(raw)`へ統合しない。current libpachaもstatusとnew-fdだけは別helperにしている (`userland/libpacha/src/syscall.c:79-85`)。`lpr_native_adopt` は expected kernel kind、minimum rights、flags を検証してから ownership を受け取る。

native namespace moduleが発行するallocation/dup/IPC receiveは常にnamed `PACHA_NATIVE_FIRST_DYNAMIC_FD`以上を要求する。0〜15を返し得るraw syscall resultをfdとして推測せず、固定/既知endpointはtyped registry entryからだけ取得する。Step 3のABI correction後もstatus-onlyの`0/+status`とnew-fdの`>=16/+status`は維持し、count/value-returnだけを`>=0 success/-status error`へ変える。これによりnew-fd allocation encodingを変えずに曖昧なlow native allocationを禁止し、Linux logical 0〜15は制限なく利用できる。

logical allocator は entry table だけを見る。logical 20 と hidden native slot 20 が同時に存在しても、型と table が異なるため衝突ではない。`close(20)` は logical entry の object を閉じ、native slot 20 を直接閉じない。fixed service endpoint 240〜246 も native registry だけが参照し、Linux app から同番号の logical fd を利用可能にする。

pipe、dmabuf、event/timer、service notify channel など現在「logical 番号 = native 番号」を仮定する object は、object-owned hidden `lpr_native_ref_t` へ移す。stdin/stdout や bootstrap 由来の native descriptor も起動時に明示 object として wrap し、「table miss なら native syscall」という fallback は除去する。

native registryはfd object外のruntime-private refsも全て所有し、上記policyとowner generationを付ける。current forkはfiled/termd wire page、filed fast session/page、readv/pread scratch VMOをservice別にunmap/closeし (`lpr_vfs/path.c:457-531`)、clone前にfiled sessionを個別dropする (`lpr_process/syscalls.c:429-436`)。これらを`RUNTIME_DROP_CHILD`または`RUNTIME_RECREATE_CHILD`へ登録し、fork coordinatorがobject snapshotと同じledgerで処理する。`OBJECT_OWNED`はobject opだけが処理して二重closeせず、fixed endpointとexec transportはnamed policyで継承/manifest化する。最終構造に`lpr_reset_fork_child_rpc_state()`型の手動service listを残さない。

上記current実装について、`lpr_vfs/path.c:457-531`が明示close/resetするのはfiled/termd wire page、filed fast session/page、readv/pread scratchである。netd cached pageは`lpr_socket.c:279-337`に別管理され、fork reset列に統合されていない。従ってnetdまで既に正しくcloseしているとは数えず、registry移行時に「未登録runtime-private ref」としてstatic/fault testで検出する。

generic OFD-anchor endpointはPhase 6完了後のfull Linux processに必須のbootstrap resourceとする。current supervisor tokenはflag付きoptionalである (`lpr_process/bootstrap_state.c:104-116`) が、process/job-control featureのoptional性とanchor transport availabilityを分離する。production/full imageでanchor endpoint欠落はbootstrapをfail closedにし、host unit用`LOCAL_ONLY` modeだけは起動を許す代わりにfork/exec/external shareを明示`EOPNOTSUPP`にする。正しいOFD共有をoptional token有無で黙って変えない。

hidden native ref の kernel CLOEXEC は内部 resource leak 防止 policy で常時設定してよいが、Linux entry の `FD_CLOEXEC` と同期しない。fork は clone transaction、exec は explicit capability manifest で必要 ref だけを渡す。`MSG_CMSG_CLOEXEC` は受信 entry の flag だけを決め、backend status や source entry を変えない。

owner/ticket lease capabilityはLinux-visible objectへwrapせず、`TRANSFER`/`INHERIT` rightsを持たないruntime-private refにする。forkでkernel cloneされたparent lease copyはchild userlandへ戻る前にdropし、prepare済みchild leaseだけをadoptする。exec targetも別leaseをmanifest化し、source leaseを継承しない。lease自体がqueue/childへ偶然残ってownerをaliveに見せる経路を禁止する。

current kernel fork clone は native fd table の全 active entryを ref-retainして子の同じ slotへ複製する (`kernel/src/state/process.zig:300-325`)。従って native-backed ops の fork prepare は同じ capabilityをさらに dupせず、clone対象を native registryから snapshotし、childでclone済み refをpolicyどおりadopt/drop/recreateする。process-create/exec の inherit判定は `INHERIT && !PRIVATE` という別経路 (`kernel/src/state/process.zig:281-297`) なので、execはこの暗黙規則へ依存せず v8 capability manifestだけを正とする。

## 4. `lpr_fd_ops_t`

### 4.1 interface

interface sketch は次とする。

```c
typedef struct lpr_fd_ops {
    uint32_t runtime_id;
    uint32_t wire_kind;
    int32_t open_priority;
    uint32_t registry_flags; /* e.g. FALLBACK_ONLY */
    uint64_t capabilities;
    uint16_t max_exec_caps;      /* declared worst case per object */
    uint16_t max_transfer_caps;  /* declared worst case per object */
    const char *name;

    lpr_fd_match_result_t (*open_match)(const lpr_open_request_t *);
    lpr_fd_open_result_t (*open_resolve)(lpr_open_request_t *);
    lpr_fd_match_result_t (*open_adopt_match)(const lpr_resolved_object_t *);
    lpr_fd_open_result_t (*open_adopt)(lpr_resolved_object_t *);
    int64_t (*read)(lpr_fd_ref_t *, void *, uint64_t);
    int64_t (*write)(lpr_fd_ref_t *, const void *, uint64_t);
    int64_t (*readv)(lpr_fd_ref_t *, const lpr_iovec_t *, uint64_t);
    int64_t (*writev)(lpr_fd_ref_t *, const lpr_iovec_t *, uint64_t);
    int64_t (*pread)(lpr_fd_ref_t *, void *, uint64_t, uint64_t);
    int64_t (*pwrite)(lpr_fd_ref_t *, const void *, uint64_t, uint64_t);
    int64_t (*lseek)(lpr_fd_ref_t *, int64_t, uint32_t);
    int64_t (*getdents)(lpr_fd_ref_t *, void *, uint64_t);
    int64_t (*truncate)(lpr_fd_ref_t *, uint64_t);
    int64_t (*sync)(lpr_fd_ref_t *, uint32_t);
    int (*map)(lpr_fd_ref_t *, lpr_map_phase_t,
               lpr_map_request_t *, lpr_map_plan_t *, lpr_fd_txn_t *);

    int (*poll_prepare)(lpr_fd_ref_t *, uint32_t, lpr_poll_plan_t *);
    int (*poll_recheck)(lpr_fd_ref_t *, uint32_t, uint32_t *);
    int (*native_wait_source)(lpr_fd_ref_t *, lpr_wait_source_t *);
    int (*epoll_register)(lpr_fd_ref_t *, lpr_epoll_reg_op_t *);

    int (*dup)(lpr_fd_ref_t *, lpr_fd_dup_context_t *);
    int64_t (*fcntl)(lpr_fd_ref_t *, uint64_t, uint64_t);
    int64_t (*ioctl)(lpr_fd_ref_t *, uint64_t, uint64_t);
    int (*stat)(lpr_fd_ref_t *, lpr_linux_stat_t *);

    int (*close)(lpr_fd_ref_t *, lpr_close_phase_t,
                 lpr_fd_txn_t *);
    int (*fork)(lpr_fd_ref_t *, lpr_fork_phase_t,
                lpr_fd_txn_t *);
    int (*exec)(lpr_exec_op_t *);
    int (*transfer)(lpr_transfer_op_t *);
} lpr_fd_ops_t;
```

phase は少なくとも以下を持つ。数値は共通化してもよいが、型と有効遷移を操作ごとに分ける。

- close: `PREPARE`, `CONFIRM`, `ROLLBACK`。rollbackはlogical commit前だけ有効。
- fork: `PREPARE`, `CONFIRM_PARENT`, `CONFIRM_CHILD`, `ROLLBACK`。
- exec: `PREPARE_SOURCE`, `ENCODE_OBJECT`, `PREPARE_TARGET`, `DECODE_OBJECT`, `CONFIRM_TARGET`, `ROLLBACK_SOURCE`, `ROLLBACK_TARGET`。成功した`PROCESS_EXEC_FROM`は旧runtimeへ戻らないためsource confirm callbackは定義しない。
- transfer: `PREPARE_SEND`, `CONFIRM_SEND`, `ROLLBACK_SEND`, `PREPARE_RECV`, `CONFIRM_RECV`, `ROLLBACK_RECV`。recv contextは`CONSUME/PEEK`を区別し、PEEKはqueue-held exportから独立target refを複製する。これらを一個の `transfer` callback が扱うため、top-level に kind 別 export/import switch は作らない。

`exec`/`transfer` は source object が存在しない decode/import phase も同じ ops record で処理するため、常に `lpr_fd_ref_t *` を第一引数にしてはならない。`lpr_exec_op_t` / `lpr_transfer_op_t` は phase、registry が選択した ops、codec、transaction、source pin（source phaseだけ非NULL）、target builderと`out_object`（target phaseだけ有効）を持つ。各phaseのNULL/non-NULL条件をstartup/unit testで検証し、wire `wire_kind`→registry→target callbackという経路だけでrestore/importする。`PREPARE_TARGET`後のdecode失敗は`ROLLBACK_TARGET`、send/encode失敗はsource側rollbackへ必ず収束させる。

すべての callback slot は非 NULL とする。未対応操作は共通 `notsup` implementation を明示指定し、read の missing callback が filed read へ落ちる、といった挙動を不可能にする。startup registry validation は wire kind の重複、必須 callback、capability と callback の矛盾、codecが実際に生成するcap数が`max_exec_caps/max_transfer_caps`を越えないことをfatalにする。上限値はframingの事後報告ではなく、prepare時のcapacity予約に使う契約である。

common core が扱うもの:

- factory resultは`NOT_MINE / OPENED(object) / CLAIMED_ERROR(errno)`のtri-stateにする。serviceがpathをclaimしてhandle作成等のside effectを始めた後のerrorは`CLAIMED_ERROR`であり、FILEDへfallbackしない。M5.3cのdrmd open後uevent `ENOENT`を「not mine」と解釈したclass (`refactor-plan.md:535-537`; `lpr_vfs/ops.c:546-566`) をこのfactory contractで閉じる。
- path文字列だけではkindを確定できないobject用にpost-resolve adopt stageを持つ。current DEVICE化はFILED open/fstat後のchar device 1:3判定であり (`lpr_vfs/ops.c:561-590`)、relative path/symlinkをpath matcherで代用してはならない。pre-resolve factoryが全て`NOT_MINE`ならFILEDがVFS resolve/openを一度だけ行い、未publishのfiled ticket + inode/backend metadataを全opsのside-effect-free `open_adopt_match`へ渡す。priorityで選ばれた一opsだけが`open_adopt`でticketをconsume/convertし、全`NOT_MINE`ならFILED opsがそのままconsumeする。ambiguous/claimed errorはticketをrollbackし、二重openやFILEDへの再試行をしない。
- `F_GETFD/F_SETFD` は entry、`F_GETFL/F_SETFL` は common OFD status、`F_DUPFD*` は table core で処理する。
- kind-specific `fcntl` は memfd seals/locks などだけを処理する。
- readv/writev の default composition は、通常 fileのようにpartial-I/O規則が同じ backendだけが明示選択できる。pipe/socketの atomic vector semanticsを core loopで分解しない。
- `map` は `PREPARE/CONFIRM/ROLLBACK/UNMAP` を持ち、generic mmap coreへ VMO、offset、rights、mapping ticketを返す。mappingは fd close後も存続できるため、object entry refとは別のmapping refをticketが保持する。`lpr_dispatch.c:568-588` の dmabuf/DRM/FILED chainをこのopへ移す。
- `dup` の default は同一 object への entry alias。callback は alias 作成の veto/補助だけであり、通常 backend ref を増やさない。

### 4.2 registration と「kind 追加 = 一定コスト」

各 backend module は `LPR_FD_OPS_DEFINE(...)` で ops record を一個定義し、linker-collected read-only section へ登録する。core は section の start/end から runtime/wire registry を構築する。手書きの中央 `ops_by_kind[]`、restore switch、epoll allowlist は作らない。

linker sectionの並び順をpath claim/adopt順には使わない。registry build時に明示`open_priority`でstable sortする。coreはside-effect-freeな`open_match`または`open_adopt_match`を該当stageの全recordへ先に行い、最優先の一recordだけのopen callbackを呼ぶ。同一priorityで複数matchはambiguous configurationとしてfail closedにし、known overlapはhost test/startup validationでも拒否する。FILEDだけが`FALLBACK_ONLY`を持てる唯一のrecordで、全non-fallback factory/adopterが`NOT_MINE`のとき一度だけ選ぶ。open開始後は`OPENED/CLAIMED_ERROR`のどちらかであり、別factoryへ進まない。このpriority/flagもbackend自身のops recordに書くため、新kind追加でcentral listを編集しない。

LPR shared object の linker が start/stop symbol retention を直接提供できない場合、同じ macro を build 時に収集して registry を生成してよい。ただし手動 central list へ fallback する案は禁止する。exact linker mechanism は Step 6 実装開始前に小さい host layout test で確定する未決事項である。

新 kind に必要なのは、payload/private state、factoryを含むops record、backend固有testである。既存core fileの編集が必要になった時点でextension invariant違反としてreviewをfailさせる。wire ABIに新kindを出す場合だけ、そのABI versionのtaxonomyを正当に更新する。

### 4.3 pin と lock

`lpr_fd_get(fd)` は table lock 下で entry/object generation を検査し `pin_count++` した `lpr_fd_ref_t` を返す。callback 中に logical entry が close されても object は pin が落ちるまで再利用しない。callback 後に `lpr_fd_put` が finalizer を進める。

fork/exec snapshot は unique object を一度だけ pin し、alias entry list を別に持つ。fd mutation は lifecycle barrier で一時停止するが、service RPC 中に fd table lock は保持しない。attempt2 のように service 別 prepare を table lock 内から呼ぶ構造、および netd queue/page lock を保持して filed RPC する構造は認めない。

M5.8Rではlock取得後にpeer exitがthread countを2→1にしても必ずlock wordをexchange-clearする修正が入った (`lpr_fd/table.c:53-66`; `tests/lpr_fd_table_test.c:163-183`)。これを現lock primitiveの前提として維持するが、lock解放後のnaked pointer lifetimeとlock中RPCは未解決なので、pin/generationの必要性は変わらない。

## 5. backend 共通 lifecycle transaction

### 5.1 transaction ledger

`lpr_fd_txn_t` は `txn_id`、用途、table generation、unique object refs、backend tickets、native refs、commit cursor を持つ。各 ticket は次を満たす。

- `PREPARE` は target 用 ref/slot/lease を予約するが、source ownership を破壊しない。
- `CONFIRM` と `ROLLBACK` は同じ `txn_id` に対し idempotent。prepare完了時点でtarget resourceは利用可能かつlease保護済みで、confirmはownership stateをdurableに確定するだけで新規allocationをしない。
- partial prepare failure は成功済み ticket を逆順 rollback。
- visible commit前の失敗だけrollbackできる。commit後のconfirm RPC timeout/peer restartは`COMMIT_PENDING`としてreaperが同じtxn idで再送し、caller-visible object/queueを取り消さない。service側のdurable txn tableまたはowner lease closeが最終状態へ収束させる。
- process が confirm/rollback を呼べず死んでも、ticket lease の peer close で service が回収する。
- service は PID 整数や `notify_pending` を alive oracle にしない。owner identity は native capability lease と generation で表す。

service protocol は共通 lifecycle request prefix `{action, phase, txn_id, owner_generation, handle}` と opaque result ticket を用い、filed/netd/drmd/inputd/termd/LPRS が同じ state machine を実装する。これは64-byte共通service envelope自体と区別する。各 service が独自名の begin/cancel/apply API を LPR top-level へ露出しない。

current netdの`reap_orphaned_sockets()`は新規open時にnotify channelのHANGUPをpollしてorphanを破棄するad-hocな前身である (`userland/netd/src/unix_socket.c:56-85`)。HANGUPはpeer capability closeの事実でありPID推測や`notify_pending`値ではないため上記原則と矛盾しないが、openの機会まで回収を遅延し、side/queue/txnを表せない。Step 16でlease close駆動の即時回収へ置換する。

### 5.2 close / dup2 / process exit

close は次の順で行う。valid entryを得た後の`close(PREPARE)`は、object内に事前確保したcleanup recordへ既存refを記録するだけのno-fail操作とする。service RPCや新規heap allocationを要求しない。

1. valid entry と object を pin し、final ref になり得る場合は `close(PREPARE)` で release ticket を確保する。prepare は irreversible backend close を行わない。
2. table lock 下で entry/object generationとrefcountを再検証する。final-close候補は `PREPARING_CLOSE` 中の新規alias attachを拒否/待機させる。finalでなくなった場合はticketをrollbackして通常のentry detachだけを行う。
3. entry を detach し、final object を `CLOSING` にする。この時点が logical commit で、同じ fd 番号は再利用可能になる。
4. 既に取得済みのnon-close pinsがあれば、close caller自身は待たず、非同期finalizerを予約して0を返す。entry detach後に新しいpinは取得できないが、close前に開始したI/Oは同じobject上で完了でき、最後の`lpr_fd_put`がfinalizerを起動する。pin drainをclose syscall threadが待つ設計は、停止したbackend I/Oとの相互待ちになるため禁止する。
5. pinsが0ならlock外で `close(CONFIRM)` を同期実行し、完了したbackendがdefinitiveなEIO/ENOSPC/EDQUOT等を返した場合はそのerrnoをcallerへ返せる。ただしfdは既にdetach済みで復活させない。transport timeout/peer restartはticketをreaperに残し、dup2 target、close_range、exit、pin残存によるdeferred finalizerのerrorはcaller結果を覆さずdiagnosticへ記録する。owner lease は最終 safety netになる。durability errorを確実に同期観測したいcallerには`fsync`を要求する。
6. logical commit 前の失敗だけ `ROLLBACK` できる。

最後entryをdetachしても`weak_watch_count != 0`なら、local object shellは`WATCH_ONLY`へ遷移し、entry refやdata-plane/backend strong attachmentを持たない。監視に必要なのはservice/LPRSのweak subscriberまたは§7のnative weak-wait subscriptionだけである。globalな最後non-watch strong rootが消えたfinal invalidationを受けた時にinterestとshellを解放する。watchの都合でdata capabilityを持ち続けたり、`refcount==0`のobjectに通常I/O pinを許したりしない。

これは Linux close の「エラー後に fd 番号を再利用し得る」性質と、backend 2-phase cleanup を両立する。current LPRもfiled backend closeのstatusをcallerへ返す経路を持つ (`lpr_fd/metadata.c:316-376`) ため、同期confirmで観測済みのerrorまで一律に握り潰さない。`dup2/dup3` はsource pinとno-fail target close-prepareを先に済ませ、target detachとsource attachを一回のtable commitにし、その後target close-confirmを行う。target backend close errorはdup2の成功を覆さず、cleanup reaperへ渡す。source validation、`oldfd == newfd`/flags規則、table reservationだけがlogical replacement前のerrorになり得る。

normal exit は table の unique object 全てに同じ close transaction を適用する。SIGKILL は userland callback を通らないため、service owner lease と native process fd-table teardown が同じ結果へ収束しなければならない。currentはM5.8Rでsocket専用cleanupを先に呼ぶようになったが、後続はFILED/DRM/INPUT/DMABUFの手動loopでTTYは対象外のままである (`lpr_process/exec.c:519-566`; `lpr_socket.c:640-655`)。このper-kind path全体を削除する。

### 5.3 fork

fork は次の順とする。

1. lifecycle barrier を取り、entry snapshot と unique object pins を作る。
2. 全 object の `fork(PREPARE)` を実行する。service-backed object は child ref/OFD attachment と child lease を予約する。native-backed object は kernel clone 後に child が adopt できる hidden capability を用意する。
3. generation を再検証する。変化していれば全 rollback して retry/error とする。
4. `PROCESS_CLONE` を呼ぶ。失敗時は parent が全 rollback。
5. parent は child が保持する lease/capability を確認して `CONFIRM_PARENT`、child は既に materialize 済みの ref を object へ差し替えて `CONFIRM_CHILD` する。child confirm は新しい remote allocation を要求せず、失敗余地を prepare 側へ寄せる。

fork ticketは`{parent_confirm, child_confirm, parent_lease_alive, child_lease_alive}`を持ち、parent/child confirmの順序に依存しない。各side attachmentはそのsideのconfirmで独立commitし、global txnは両sideが`confirm済み`または`lease deathで解決済み`になった時点でretireする。片側がconfirm前に死ねばその側の予約だけをabortし、既にconfirmしたchild/parent refと生存側source refは壊さない。parentはchildのuserland実行を待つ必要がなく、childはprepare済resourceのadopt以外のRPC allocationを行わない。

filed handle が process/session local でも、child handle は同じ underlying OFD を指し、offset/seals/status を共有する。socket も同じ netd open socket description を参照し、wait/notify endpoint だけ child owner 用に attach する。parent close と child post-fork DUP の race は構造的に消える。

### 5.4 exec

exec は「旧 image の backend ref を閉じてから同じ数値 handle を restore」してはならない。

1. non-CLOEXEC entry と unique object を snapshot/pin する。alias は object id で保存する。
2. `exec(PREPARE_SOURCE/ENCODE_OBJECT)` がmanifestを作る。preserve entryだけでなく、生存EPOLL graph等から参照されるweak watched objectまでgraph closureを取り、unique objectを一回だけencodeする。
3. source runtime内でtarget registryを引き、全framing/blob/capability rightsを検証して`PREPARE_TARGET/DECODE_OBJECT`をstaging builderへ実行する。builderの実体はsource heapではなく、filedがstaged target address spaceへ予めmapし、sourceにもbuild中だけ一時mapするsealed target-state VMOとする。table/object/arena backing、builder offset、予約slotはこのVMOへ配置し、kernel exec commitでtarget mappingととも存続する。同等のkernel-owned staged pagesへ変更する場合も、source replace後のtargetがallocationなしでadoptできる同じ存続契約を必須とする。decodeはそのobjectのentry数とgraph edgeのroleを受ける。non-CLOEXEC entry、mapping、SCM queue等のnon-watch rootがあるobjectだけにtarget data-plane/backend strong attachmentをmaterializeし、entry 0でweak edgeだけのobjectは`WATCH_ONLY` shellとweak subscriberだけをmaterializeする。weak-only recordに通常attachment/data capを作って自分自身をstrong rootにすることを禁止する。owner lease、logical table/object容量、local mutable stateを含め全てmaterialize/reserveし、ここまでtableへpublishしない。外部RPC、新規capability allocation、失敗し得るsemantic decodeはkernel commit前に完了させる。
4. target capabilityは専用manifest channelへ`BEGIN / <=19 capsのCHUNK / COMMIT`でMOVEし、COMMIT後はsenderを閉じてbundleをsealする。kernel queueは16 messages、messageあたり19 capsである (`kernel/src/state/types.zig:419-424`) ため、BEGIN + 最大14 CHUNK + COMMITでnative table全256 slots分を表現できる。実install上限はstaged targetのmandatory slotsを除く空き数で、preflight時に`capability_count <= target_free_slots`を検査する。cap ordinalはraw fdでなくbundle内受信順を指す。
5. filedが作るstaged processのnative fd tableにはtarget bootstrap/fixed endpointsとbundle receiverだけを置く。kernel atomic commitはaddress space/VMA/contextとともにこのstaged fd tableをcurrent principalへmoveし、旧source fd tableをdeferred cleanupへmoveする。旧hidden refsを数値slot単位で継承せず、survivorはmanifestだけが決める。現16 inherit fds/4 patch上限はbundle bootstrapにだけ使い、v8 object数/cap数の上限にはしない。
6. exec 準備またはkernel atomic commitが戻って失敗した場合、source table/address space/native fd tableは未変更のまま、source側の一時mappingとtarget-state VMO、sealed bundle、source/target ticketsを逆順rollback/破棄する。
7. kernel commit後、新runtimeは自分のaddress spaceに既に存在するsealed target-state VMOのbuilder offset/slotsをadoptし、bundleをdrainする。prevalidated blobとcapsをno-new-memory/no-new-remote-allocationで結び、全object/entry/weak graphを一回だけtableへpublishする。post-commitのmemory corruption/内部不変条件違反はtarget ticketsをrollbackしてnew imageをfail closedで終了し、旧imageへは戻らない。通常failureをこの経路へ持ち込まないよう、必要memory/table/native slotsはすべてprecommit stagingで予約する。
8. new runtime がatomic install後に `CONFIRM_TARGET` する。source retirementはkernel exec commitで旧owner lease/old fd tableが閉じることをimplicit confirmとし、旧runtimeのcallbackを必要としない。target confirm前の死亡はtarget lease closeでabortする。
9. CLOEXEC entriesはtargetに作らない。同じobjectにnon-CLOEXEC aliasまたはsurviving graphからのweak watchがあればobject recordは一回だけpreserveし、CLOEXEC aliasesだけを落とす。entry 0個のweak-watch object recordも許す。preserve root/edgeがないsource objectと旧owner refsはexec commit/owner-channel closeでreleaseする。preserved target refsとは別ticketなので、最終source ref closeがtarget handleを無効化しない。

## 6. transferable-FD 契約

### 6.1 opaque capsule

wire capsule は概念上次を持つ。

```c
typedef struct lpr_transfer_capsule {
    uint32_t version;
    uint32_t wire_kind;
    uint32_t batch_object_id;
    uint32_t payload_bytes;
    uint32_t capability_count;
    uint64_t required_backing_rights;
    uint64_t txn_id;
    uint64_t delivery_anchor;
    /* opaque backend bytes + capability ordinals */
} lpr_transfer_capsule_t;
```

一つのancillary batchはdescriptor occurrence列とunique object capsule列を分ける。occurrenceは送信配列順の`{batch_object_id, effective_rights}`を持ち、同じfdまたはdup aliasが複数回現れた場合も一つのcapsuleを参照する。unique capsuleは全occurrenceに必要なproof済みbacking authorityのunionを一回だけ運べるが、receiver entryのrightsは各occurrence値を実capability/backend ceilingでintersectionしたものになり、別entryへ波及しない。netd が理解するのは version、size、count、batch内id整合性、txn/lease、delivery anchorだけである。`wire_kind` と opaque bytes は改変せず保持し、LPR registry が receiver 側 ops を選ぶ。metadata の raw handle/rightsを信用せず、実 native capability の kind/rights/size と service ticket を receiver ops が再検証する。

### 6.2 send transaction

1. LPR は全 cmsghdr の長さ/alignment と fd array を検証し、descriptor occurrence順を保存しつつsourceをobject identityでgroup化し、全unique objectを一回だけpinする。一個だけに限定しない。各source entryの`TRANSFER`とeffective rightsを先に検査する。
2. stream data長0ならfd batchをqueueせず0を返す。長さがあるstreamはnetdに現時点でatomicに受理できるdata prefixをreserveさせ、0 bytesなら`EAGAIN`等で全rollback、1 byte以上ならそのprefixの先頭へancillaryを一度だけ結ぶ。datagramはmessage全体をall-or-nothingでreserveする。
3. 各unique object の `transfer(PREPARE_SEND)` が duplicate reference、opaque ticket、必要 native caps とoptional generic `queue_dependency` tokenを一回だけ作る。sender object は変えない。socket opsのdependency tokenはnetd lifecycle tableが発行し、queue brokerはkind/blobを解釈せずgeneric edgeとして保持する。
4. 全 prepare 成功後に capsule とreserved data prefixを queue reservation へ commit する。netd が ownership を受けた時点で各 ops の `CONFIRM_SEND`。失敗は reverse rollback。full requestを収容できなくても1 byte以上commitしたstream sendはそのpartial byte countを返し、fd batchを二度queueしない。
5. stream capsule の `delivery_anchor` は最初にacceptedとなったpeer absolute rx byte offsetに、datagram は message sequence にする。先行 data だけを読む recv は ancillary を消費しない。このzero/partial規則はupstream `net/unix/af_unix.c:2376-2518`を互換基準にする ([Linux source](https://github.com/torvalds/linux/blob/master/net/unix/af_unix.c#L2376-L2518))。

### 6.3 receive transaction

netd delivery は `RECV_PREPARE` と `RECV_COMMIT/ABORT` の二段にする。prepare 中は queue cursor を進めず、delivery lease を作る。

1. LPR はoccurrence順に必要unique object、native caps、backend tickets、logical entry slotsを計画し、logical/native/serviceの全容量を先にreserveする。3〜15を含む最小空きlogical fdを利用できる。いずれかの容量不足なら全層でinstall可能な最大occurrence prefixを確定する。
2. registryでopsを引き、delivery対象unique capsuleごとの`PREPARE_RECV`がcapability kind/rights、backend ticket、`ofd_key` proof、metadataを検証する。同じkeyが既存objectにあればcanonicalizeし、なければtarget objectを一回だけ作る。occurrenceのbatch object idはこのobjectを参照する。
3. 検証/prepareしたprefixだけをatomic installし、同一capsule/keyを指す複数entriesは同じreceiver objectへaliasする。prefixがoccurrence `[A,A]`の途中で切れても、retained occurrenceが一つ以上あるA capsuleのbackingは一回だけobjectへadoptし、suffixはentry intentだけを捨てる。retained occurrenceが0のunique capsuleだけをrollback/closeする。netd deliveryとbackend ticketsをこのunique-capsule accountingでconfirmする。
4. `MSG_CMSG_CLOEXEC` は新 entry 全てへ `FD_CLOEXEC` を set する。OFD status、source entry、hidden capability policyは変えない。
5. control bufferまたはlogical/native/backend capacity不足でもdataは受け取り`MSG_CTRUNC`をsetする。完全な`int` fdとして収まりinstallできるprefixだけを返し、CONSUME時は上記accountingでsuffix occurrence/capsuleをdiscardする。`cmsghdr`自体またはfd一個も完全に入らなければcontrolを全discardする。`EMFILE`をrecvmsg全体のerrorにしてdataを再配送せず、0個prefixでもdata成功+`MSG_CTRUNC`にする。
6. unknown wire kind、不正rights/size/proof、backend protocol errorはentryを一個もpublishせず全prepared stateをrollbackし、capsuleをfail closedでquarantine/closeする。transient service errorはdelivery cursorを進めずretry可能なerrorにする。どちらもcapability ownerは一箇所で一回だけcloseする。

plain `read/recv` またはcontrolを渡さない非PEEK `recvmsg`がanchor byteを消費した場合、dataは返すが対応capsuleをimportせずdiscardする。current codeもcontrolがない場合に受信handleをdiscardする (`lpr_socket.c:1259-1312`) が、generic queueではbyte anchor単位で同じ規則を適用する。

Linux互換の`MSG_PEEK`はqueue data/cursorと元export ticketを残したまま、そのpeek呼出し用に各OFD refを複製し、新しいlogical fdを返す。従ってpeekを繰り返すたび独立receiver entries/refsが増え、その都度`FD_CLOEXEC`、control/slot truncation、rollbackを適用する。PEEK suffixのcloneだけを破棄し、queue原本は後のPEEK/CONSUME用に残す。非PEEK consumeが元queue ownershipを最後にreleaseする。peek/no-controlは原本をdiscardしない。capacity testはrepeated PEEK後の全fd closeと最終consumeでbaselineへ戻ることを要求する。

current M5.8Rとの距離は明確である。netdのPEEKはdata copyだけでancillary refを呼出し側へcloneせず (`userland/netd/src/unix_socket.c:192-206`)、LPR `FIONREAD`はその`PEEK|DONTWAIT`をbyte-count probeに使うだけである (`lpr_socket.c:1614-1653`)。従って現greenはdata cursor不変だけの証明で、repeated fd clone、CLOEXEC、capacity rollback、最終consumeの証明ではない。これらをStep 21のred/acceptanceにする。

streamの一回のrecv/recvmsgはordinary byte prefixを読んで最初の`delivery_anchor`へ到達できるが、そのfd-bearing send batchを処理した所で停止し、同じcallで次のsendmsg由来のfd batchへ跨がない。大bufferや`MSG_WAITALL`でも同じで、PEEKも最初のbatchをcloneした所で停止してcursorを進めない。同一sendmsg内の複数cmsghdr/occurrencesだけが一batchである。これはupstreamのnon-PEEK detach後/PEEK clone後の`if (scm.fp) break` (`net/unix/af_unix.c:3047-3073`) を基準にする ([Linux source](https://github.com/torvalds/linux/blob/master/net/unix/af_unix.c#L3047-L3073))。ordinary prefix → fd batch A → fd batch Bを一つのlarge recvへ渡し、Aまでで止まり次callがBを返すoracleを固定する。

Linux互換のsendmsg上限は`SCM_MAX_FD=253`に固定し、一sendmsg中の累積fd数が254以上なら`EINVAL`にする。これはupstream Linuxの定数`include/net/scm.h:15-18` ([source](https://github.com/torvalds/linux/blob/master/include/net/scm.h#L15-L18))、`scm_fp_copy()`の単一/累積上限`net/core/scm.c:69-105`、prefix/`MSG_CTRUNC`/`MSG_CMSG_CLOEXEC`/cloneの`net/core/scm.c:343-397` ([source](https://github.com/torvalds/linux/blob/master/net/core/scm.c#L69-L105)) を互換基準にする。

Pacha kernel IPCの19は「一native messageのcapability数」、queueは16 messagesにすぎず (`kernel/src/state/types.zig:419-424`; public定数は `userland/libipc/include/pacha/ipc.h:13-20`)、SCM fd上限にはしない。brokerはdescriptor/cap総数とfinal socket slotを一high-level txnでreserveし、prepared capsを一個以上のsealed cap-bundle channelへ`<=19 caps`ずつMOVEする。bundle chainのkernel queuesがqueue-held strong refsを所有し、netdはchain root/leaseだけを持つため253 unique capsをnetd native fd tableへ展開しない。PEEKはgeneric bundle `CLONE`、CONSUMEは`SPLIT_PREFIX/MOVE`を、chunkをbounded scratch slotsへ逐次drain→dup/move→resealして実装し、kindを解釈しない。全bundle seal後だけcompact ticketをAF_UNIX queueへatomic publishし、途中失敗/peer deathは全chainをcloseしてcapsを解放する。一logical fdが複数capsを使うbackendはregistryの`max_transfer_caps`を申告し、必要bundle数/transaction capacityをBEGINで先にreserveする。exact chain/page framingと総cap budgetはStep 21の未決だが、このspool ownershipとatomic publishは確定事項である。testはfd数0/1/19/20/253/254（254は`EINVAL`）、253 unique caps、same-fd 253 occurrences、cap-heavy backend、near-full receiver native tableを含める。

### 6.4 rights と lifetime

- receiver entry rights はそのdescriptor occurrenceのsource entry rights、実 capability rights、object ceiling、backend export policy の積集合とする。unique objectへcanonicalizeしても他entry rightsとunion/intersectionしない。metadata から権限を足さない。
- source entryの`FD_CLOEXEC`は転送表現へ入れない。receiver entryはdefault clearで、`MSG_CMSG_CLOEXEC`指定時だけsetする。OFD status/access modeは同じobject stateを共有する。
- retransfer に必要な `TRANSFER` right は source が持つ場合だけ保持する。低 logical fd かどうかは rights と無関係。
- send 成功後に sender が close しても queue ticket が OFD/backend/native ref を保持する。
- receive 成功後は receiver object が保持し、queue は ownership を手放す。
- source/receiver/queue の SIGKILL は lease close と kernel capability teardown で回収する。`notify_pending`、queue に残った HUP message、PID の存在は liveness ではない。

socket transferだけはqueued ticketが別socketのqueueを所有し得る。socket opsの`PREPARE_SEND`はnetd自身のlifecycle tableからopaqueなgeneric `queue_dependency` node/edge tokenを受け取り、brokerはcapsule kind/blobをparseせずqueue recordへ結ぶ。rootはprocess ownerだけでなく、listener/internal owner、in-flight fork/exec/recv ticket等を含む全non-queued strong leaseとする。queued-transfer tokenはedge、epoll weak subscriberは非rootである。netdはowner close/queue mutation時にbounded graphをmark-sweepし、rootから到達不能なqueued-only SCCをclearして全bundle/caps/ticketsをreleaseする。wall-clock timeoutや`notify_pending`で破棄しない。self-send、A↔B cycle、cycle保持processのSIGKILL後にsocket/queue/capacityが回収されるoracleなしにsocket transferをenableしない。

### 6.5 backend ごとの表現

| backend | Phase 6 の transfer 表現と可否 |
|---|---|
| filed memfd | `filed` が同じ OFD と canonical shared VMO を export-reserve。receiver filed session へ同じ OFD attachment を importし、canonical VMO cap、size、access rights、seals を検証する。runtime kind は引き続き FILED ops。M5.6 の本実装。 |
| filed regular file | contract は同じだが、Phase 6 で memfd 以外まで enable するかは未決。未実装時は `EOPNOTSUPP` を明示し、memfd 判定を path や size で推測しない。 |
| native pipe endpoint | data endpoint capability/direction/rightsと、object生成時のarena `ofd_key` + LPRS slot proofを別ownershipで運ぶ。logical fd numberは運ばず、LPRSはdata endpointをretainしない。receiverはticket/cap chainを検証してPIPE ops objectへcanonicalizeする。 |
| socket | netd open-socket-description reservation tokenとstable OFD key。receiver用notify/wait channelを新たにattachし、senderのwait fdを公開しない。statusは同じsocket OFDを共有する。queued-only cyclesは§6.4のgraph GC対象。 |
| INPUT / DRM | 現 M4 の転送を generic ops へ移す。service handle ref を reserve し、receiver 用 wait endpoint を attach する。netd wire から kind-specific fields を除く。 |
| TTY | fork/exec lifecycle は実装する。SCM_RIGHTS は owner/session semantics の test ができるまで明示 `EOPNOTSUPP`。 |
| eventfd/timerfd | current local counter/deadline は cross-process OFD を表せないため、native/shared object 化まで transfer disable。 |
| dmabuf | native VMO と drmd PRIME token の一体 transaction が必要。Phase 6 filed memfd の受け入れには含めず明示 disable。 |
| epoll/device | epoll interest graph、stateless device の再構築 semantics を別 test で固定するまで明示 disable。 |

「generic contract」は全 kind を無条件で transfer 可にする意味ではない。各 ops が可否と表現を一箇所で明示し、不可 kind が別の fallback kind へ化けないことを意味する。

## 7. poll / epoll 統合

`poll_prepare` は requested Linux events に対し、`READY(revents)`、`WAIT(wait_source, native_events, cookie)`、`DEAD(POLLNVAL)` のいずれかを返す。`native_wait_source` は common `wait_native` を borrowed pin として返し、raw int ownership を移さない。

poll planner は次の順で動く。

1. 全 fd を pin し、全 ops の `poll_prepare` を呼ぶ。ready が一つでもあれば直ちに revents を返す。
2. WAIT sourceをnative ref identity（slot + registry generation）でdedupeし、一つのsourceから全logical poll occurrenceへのfan-out listを作る。同じsocket/OFDのdupを数百個pollしてもnative entryを重複登録しない。
3. unique WAIT sourcesを一つの native `fd_wait_many` set に集める。kernel上限はnative fd tableと同じ256である (`kernel/abi/fd_abi.zig:1,155`) ため、全active native refsをdedupeした集合は上限内に収まる。opsがnative registry外のsourceを返すことは禁止し、overflowはsilent chunk pollingで回避せずinternal invariant failureにする。timerfd/local deadline は最短 timeout として統合する。
4. wait 中は table lock を持たない。wake/timeout/signal 後、pending signal delivery を行ってから、wake sourceに紐づく全logical occurrencesのobject generationを検証して`poll_recheck`する。timeout/signal時は必要な全occurrenceをrecheckする。
5. close/reuse された object は `POLLNVAL`、epoll pin が生きる object は registration semantics に従う。

final structure では potentially blocking backend は native wait source を提供する。pipe/event/timer は object-owned native capability、DRM/input/socket は service notify channel、tty は service wait endpoint を使う。filed regular file や `/dev/null` のように常時 ready の object は wait source 不要である。10ms sleep/recheck は移行中 adapter にだけ許し、wait endpointが揃う Step 16 終了時に削除する。通常のpoll/select中はobject pinそのものnon-watch strong rootがあるので、その期間だけobject-owned sourceをborrowできる。

一方、epoll interestはdescriptor close後も残り得るため、object-owned native capabilityのdupをpersistent watchに使ってはならない。current kernelのpipe endpointはkernel object payloadが最終releaseされた時だけ`read_refs/write_refs`を減らす (`kernel/src/state/fd.zig:277-317`; `kernel/src/state/pipe.zig:192-199`)。watchがstrong capを持つとlast reader/writerが消えず、EOF/EPIPEとautomatic epoll removalが永久に来ない。そこでnative-backed targetの`epoll_register` は、§9.4のgeneric weak-wait subscriptionを作る。subscriptionは`{source object ref, source generation, event mask}`をkernel内でweakに参照し、sourceをretainせず、read/write/transfer/inherit authorityを持たない。readiness changeでwaitableになり、最後のsource strong ref解放時はHANGUPをpublishしてsourceからdetachする。closeはback-linkを外す。service-backed targetは同じ意味のreceiver-specific weak subscriber ticketを返す。LPR/LPRSの数え上げだけでnative strong capの寿命を弱参照化したことにはしない。

epoll interest は `(ofd_key, registration fd, local object generation, events, data, edge/oneshot state)` を保持する。poll/select可とepoll target可は同じでない。例えばregular fileはpollでは常時readyでもepoll ADDは`EPERM`になり得るため、opsは`POLL`と`EPOLL_TARGET` capabilityを分け、`epoll_register` callbackがobject metadataごとに許可/`EPERM`、nested policyを決める。中央kind allowlistは持たない。epoll object自身のshared graph/wait attachment cleanupはepoll opsのfinal closeが行う。

同じ OFD の dup を epoll へ登録した Linux semantics を保つため、registration fd と`ofd_key`の両方をkeyにする。interestは`weak_watch_count`とbackend/LPRSのweak subscriber ticketを持つが、OFDをaliveにするstrong rootには数えない。logical descriptor attachments、SCM queue/export ticket、mapping、in-flight prepared target等のnon-watch strong refsをbackend/LPRSがglobalに数え、同じ`ofd_key`の最後のstrong refが0になった時だけ全subscriberへfinal invalidationを送り、interestを削除してwaiterをwakeする。last local entryだけでは削除せず、epoll自身のwatch ticketでlifetimeを延ばさない。wait/recheck中だけ一時pinを取り、table file index再利用をidentityに使わない。

従ってfork先やSCM queue/receiverが最後のstrong refを持つ間、元processが全entryをcloseしてもinterestは残る。最後の他process/queue ref closeで初めてremoveされる。Step 12でsingle-process weak watch、Step 17でshared EPOLL graph、Step 22でSCMを含むglobal-last-ref oracleを順に有効化する。

## 8. exec fd-table ABI v7 への影響

### 8.1 v7 を維持できない理由

現 v7 は一 entry 48 bytes の `{fd, kind, flags, handle, offset_or_counter, native_wait_fd}` である (`userland/filed/include/filed/payload.h:318-334`)。filed は kind switch で semantic validation し (`filed/src/dispatch/transport_reply.c:352-423`)、同じ六 field を bootstrap へ copy する (`transport_reply.c:481-492`)。LPR image ABI は 10、bootstrap entry size は 48 (`personality/lpr_image_abi.h:11,91-124`)。さらにcurrent exec transportはinherit fds 16、inherit handles 4、fd patches 4に固定される (`filed/payload.h:46-48`) ため、ordinal recordを足すだけでは17個目のnative refをtargetへ渡せない。

この形式では次を表せない。

- 複数 entry が一つの OFD object を alias すること。
- backend-private variable payload と transaction ticket。
- raw slot numberではない native capability ordinal。
- common status/backend ref と entry CLOEXEC の分離。
- filed が kind を知らず opaque に transport する境界。

従って common field migration 中は wire layout を変えず、Step 19ではgeneric exec coordinatorをv7 framing adapter上へ先に載せる。entry/object graphを表せるようにするStep 20でだけv8へflag-day updateする。

### 8.2 v8 案

v8 は header、entry records、object records、backend blob、capability ordinal table を分離する。

```text
header  : magic, version=8, header/entry/object sizes, total bytes,
          entry count, object count, capability count, txn id
entry[] : linux fd, snapshot object id, fd_flags, effective rights
object[]: snapshot object id, wire_kind, access mode, status representation,
          required backing-rights ceiling, backend blob offset/size,
          capability ordinal range
blob    : ops->exec(ENCODE_OBJECT) が作る opaque data
caps    : raw fd numberではなく manifest bundle capability ordinal
```

snapshot object id はその manifest 内だけで有効な連番であり、runtime file index や pointer ではない。restore は object を一回 decode/import し、複数 entry を同じ object へ attach する。filed は bounds、record ordering、重複 object id、cap ordinal range だけを検証し、wire kind の handle semantics は解釈しない。LPR ops decoder が fail closed で検証する。

source table generationはsnapshot/revalidate ledger内だけに置き、wireへ出さない。target generationはinstall時に新規採番するため、source runtime値をentry recordへ保存しても意味がなく、ABA defenseにもならない。

`status representation`はlocal valueまたはbackend-canonical referenceというtagged表現であり、remote OFD status/offsetを独立したwritable copyとして複製しない。filed/socketのtarget attachmentが同じbackend OFDを保持し、manifest blobにはその一回限りticket/proofだけを入れる。

entry effective rightsはaliasごとに保存する。object側のrequired ceilingはtarget cap/service ticketの検証要求であってauthorityそのものではなく、targetは実capability/backend policyから得たceilingを超えるentry recordをrejectする。同じobject idを指すentry rightsをunionして全aliasへ適用してはならない。

restoreは二passにする。第一passで全object shell/backend attachmentを作り、第二passで他objectを参照するblob/weak watchをsnapshot object idから解決する。現epollが持つ`target_file_index/target_generation` (`lpr_epoll.c:38-45,294-317`) はruntime再利用検出には使えるがwire identityにはしない。Step 17でcreation-time arena/shared-stateへ移行済みのEPOLLはinterest graphを複製せず、LPRS shared-state ticket/proofとprocess-local wait attachmentだけをrestoreする。manifest local idはgraph closure/weak watched objectを同定し、entry 0個のobject recordも許す。未解決id、重複registration、不正cycle/depthはtable publish前にrejectする。

capability ordinalの実体は§5.4のsealed manifest channelで運ぶ。v8 high-level transactionはmanifest bytes、target table slots、全cap refsを先にreserveし、`BEGIN/CHUNK/COMMIT`受領完了後だけ`PROCESS_EXEC_FROM`へ進む。一chunkの19 capはtransport単位にすぎず、16/4という旧filed配列上限をobject/cap上限にしない。mid-chunk failure、source/target death、unknown ordinalはbundleと全ticketをrollbackし、source imageを変更しない。exact record/chunk bytesは未決だが、このmulti-message atomic stagingは確定事項である。

Step 20 では次を同時に行う。

- `FILED_EXEC_LPR_FD_TABLE_VERSION` 7 → 8。
- bootstrap layout が変わるため `LPR_IMAGE_ABI_VERSION` 10 → 11。
- v8 taxonomy は意味上の group 順に全番号を定義し直す。旧番号を保つため新 kind を末尾に足さない。
- v7 reader/writer、old kind shim は置かず、version mismatch は現在と同様 fatal/reject (`bootstrap_state.c:23-101`)。
- syscall numberの変更はない。kernel は manifest の kind/blob を解釈しない。
- transport oracleはcap数0/1/16/17/19/20と`target_free_native_slots`/`+1`、alias多数だがcap一個、各chunk後failureを含める。logical entry数はnative slot数で制限しない。

exact byte offsets、alignment、最大 blob/cap count は Step 20 開始時に `run-userland-service-abi-layout.sh` の red test として先に固定する未決事項である。

## 9. service / kernel への波及

### 9.1 filed

必要な変更:

- owner/OFD ref に共通 `PREPARE/COMMIT/ABORT` と capability lease を持たせ、fork/exec/transfer target attachment を idempotent にする。
- `FILED_OP_VFS_DUP/CLOSE` (`filed/ipc_protocol.h:31-35`) を local logical dup のたびに呼ばず、unique object lifecycle に限定する。
- `filed_handle.fd_flags`を削除するかwire request-local fieldへ降格し、LPR entry以外を`FD_CLOEXEC`のsourceにしない。filed OFDの`status_flags`は逆にbackend canonical stateとしてLPR objectから参照する。
- memfd OFD と canonical VMO の export/import、rights/size/seals validation を実装する。既存 `SHARED_FILE_VMO` と `MEMFD_CREATE` の ownership を再利用する (`filed/ipc_protocol.h:46-55`)。
- exec v8 は opaque manifest transport にし、filed の kind switch validation を除去する。
- dead owner/session の `CLOSED/HANGUP` で handle、mapping、pending ticket を回収する。

不要な変更:

- ext4 VFS/journal、koboxd fs backend、`_kobox`、path lookup へ fd kind/SCM knowledge を入れない。
- filed が netd queue や Linux cmsghdr を解釈しない。

### 9.2 netd

必要な変更:

- AF_UNIX queue を byte-sequence + ancillary capsule list にし、複数 capsule、absolute byte anchor、delivery transaction を持つ。
- opaque capsule/sealed cap-bundle chain/root leaseのownershipを持ち、kind-specific `fd_handle/fd_aux` を削除する。全capsをnetd native tableへ展開しない。
- socket OFD の ref reservation、socketpair、fork/exec/transfer attachment、receiver-specific notify channel を共通 lifecycle protocolへ載せる。
- owner lease peer-close を liveness の正とし、socket、queued caps、notify endpoint を回収する。generic `queue_dependency` graphでqueued-only socket SCCを回収し、brokerはwire kind/blobを解釈しない。
- external service RPC が必要なら queue lock を解放し、ticket/generation で再検証する。filed import/export は原則 sender/receiver LPR が行い、netd は filed を呼ばない。

不要な変更:

- TCP/UDP/uinet data path、routing、device model に LPR fd kind を入れない。
- `notify_pending` を alive flag として拡張しない。current field/write-clear (`netd/src/unix_socket.c:13-46,154-165,192-225`) は notification coalescing にだけ使う。M5.8Rの`reap_orphaned_sockets()` (`unix_socket.c:56-85`) はnotify peerのHANGUPを見るのでPID/flag推測ではないが、Step 16のowner lease導入時に置換して残さない。

### 9.3 drmd / inputd / termd / LPRS と service ABI

三 service は既に close/dup 相当を持つ (`drmd/ipc_protocol.h:11-22`, `inputd/ipc_protocol.h:8-16`, `termd/ipc_protocol.h:12-25`)。これを top-level の手動呼出しとして増やさず、共通 lifecycle request と owner lease に合わせる。

- drmd/inputd は receiver/fork child 用 handle ref と notify endpoint attachmentを transaction 化する。device ioctl/read/event generation は変えない。
- termd は tty handle の fork/exec/closeを transaction 化する。PTY/job-control/signal protocol は変えない。
- dmabuf transferを後で enable するときだけ drmd PRIME token + VMO の一体 ticket を追加する。
- LPRS はprocess supervisor機能と混ぜたkind switchを持たず、local/native objectをcross-process shareするときのgeneric OFD-anchor tableを共通transaction/lease moduleとして所有する。anchorはidentity/state arena slot、shared status、stable generation、strong/weak countsを結び付けるが、pipe/data endpoint capabilityを常駐retainしない。export時にtrusted LPRがdata cap ordinalとslot-scoped one-use ticketを同じtxnへ結び、bare `FD_GET_INFO` metadataをidentity proofにしない。

current共通service ABI versionは2で、request/reply magicも`PACVREQ2`/`PACVRPY2`に相当する末尾`0x32`を含む (`userland/libipc/include/pacha/service_abi.h:7-11`)。service op ABIを変えるcutではversionを2→3、両magicを`...3`へ同時更新する。version文字を埋め込む`PERSONALITY_LPR_NOTE_MAGIC`、`PERSONALITY_TRAP_FRAME_MAGIC`、`LPR_RUNTIME_MAGIC`も末尾2→3へ同じflag-dayで更新する (`personality/personality_abi.h:8-12`; `personality/runtime_page.h:7-8`)。既存`DUP/CLOSE`自体の意味をlease-backed transactionへ変え、全対象requestに`txn_id/owner_generation/phase`を持たせるため、単なる末尾op追加では表現できないのがbumpの理由である。fd lifecycleが変わるfiled/netd/drmd/inputd/termd/LPRSのop enumはlifecycle / I/O / transfer / diagnosticsの意味group順に全て振り直し、v2 compatibilityを持たない。

v2 common service idsにはNETDがなく、INPUTDもprivate `0x494e5055`を使う (`service_abi.h:14-19`; `inputd/ipc_protocol.h:8-16`)。v3では`FILED, STORAGE, TERMD, DRMD, INPUTD, NETD, LPRS, LPR_CLIENT`を共通taxonomyに置き、INPUTD/NETDを意味group位置へ挿入して後続id/error-domainを全てずらす。netd/inputdも共通envelopeを使い、raw/private id compatibility pathは置かない。transfer用op/structはこの一回で定義し、後続transfer Stepで有効化することでABI bumpを繰り返さない。

共通envelopeは現在64 bytesで、その後ろのpage payload offsetも64で固定される (`service_abi.h:11,39-66`)。一方netd wire pageは65,536 bytes、I/O payloadは`65,536-256`である (`netd/ipc_protocol.h:38-39,95-104`)。v3でenvelopeを拡張するか、64 bytesを維持して共通lifecycle prefixをpayload先頭へ置くかはStep 13冒頭のlayout redで一つに固定する未決事項である。common validatorはservice descriptorからpage/header capacityを受け、netd data pageを誤って8KiBへ縮小せず、他serviceを一律64KiBへ拡大もしない。request/reply magic、header bytes、payload offset/maximum、txn field offsetsを同じ数値表で検査し、serviceごとの隠れた別layoutを許さない。

`PACHA_SERVICE_ABI_VERSION`は`PERSONALITY_ABI_VERSION`と`FILED_FAST_VERSION`にもaliasされる (`personality/personality_abi.h:6-8`; `filed/payload.h:16-24`)。従ってflag-dayではfiled/filed_smoke、koboxd control/ipc、seed0boot/seed0root、lpr_supervisor、personality/LPR client、netd、termd、drmd、inputd、personality note/trap/runtime magicのloader/kernel/runtime consumer、およびlayout testを一括rebuild/updateする。koboxd storage/ext4 data-plane op enumやfs backendはfd lifecycleの意味が変わらないため、global version/magic/headerに合わせるだけで不必要にrenumber/editしない。現在のM5.6b journal差分へ実装を重ねるのはbaseline確定後だけである。

### 9.4 kernel

fd ops、logical table、service handle transaction は userland の責務である。native fd windowは新syscallを作らず既存constant/get-infoをtyped userland APIに包み、`fd_wait_many`も既存syscallを使う。ただし現return domainの衝突はuserlandで判別不能なので、下記2のerror-sign correctionだけはkernel ABI意味変更を要する。

kernel 変更が正当化されるのは次の六点だけである。実装時は AGENTS.md に従い、各 Step の red test と理由を示して事前許可を得る。

1. **signal pending-frame delivery/order と process-directed owner**: LPR runtime 中に抑止された native signal を guest syscall return frame へ反映し、process-wide signalの配送threadを選ぶのはtrap frame/thread tableを所有するkernelでしか完結しない。current ABIはM5.6Rで`REGISTER=1, RETURN=2, DELIVER_PENDING_FRAME=3`を末尾追加している (`kernel/abi/process_abi.zig:74-76`)が、Step 1で`REGISTER=1, DELIVER_PENDING_FRAME=2, RETURN=3`へ意味順に全consumerを一括変更し、old sub-op compatibilityは持たない。`REGISTER`はfull LPRの初期/event-loop threadをprocess-directed delivery ownerとしてgeneration付きで登録し、worker cloneはhandler設定を共有してもownerを暗黙継承しない。currentの「最初のblocked thread」選択 (`kernel/src/scheduler_connection.zig:2255-2288`) をowner wake/deliveryへ変え、owner exit/re-registerはfail-closedなstate transitionとする。M5.6Rのpublish-before-wake (`scheduler_connection.zig:2260-2277`) は維持する。syscall number 12は不変とし、Sway TERMの`escalated=1`を再現するred、owner/worker双方がblockedでもownerに一回だけhandlerが届くred、ABI layout redを先に固定して許可を取る。
2. **native value/count return domain**: kernelだけがsuccess value 1〜6とerror status 1〜6の発生点を区別できる。`FD_READ/WRITE/READV/WRITEV` (`kernel/src/syscall/fd.zig:131-234,252-367`)、`FD_POLL/FD_WAIT_MANY` (`kernel/src/syscall/fd.zig:472-515`)、`FD_FCNTL(GET_FLAGS)` (`kernel/src/syscall/fd.zig:370-382`) とvalue-returning `FD_IOCTL` (`kernel/src/syscall/fd.zig:940-979`) の全errorを`-status`へし、nonnegativeをsuccess value/count専用にする。fcntl/ioctlはcommand/request別typed decoderを使う。status-onlyは`0/+status`、new-fdは`>=16/+status`の現domainを保つ。syscall番号/argument layoutは不変だがreturn意味は非互換なので、Step 3でkernel/libipc/LPR/service全callerを一括更新しold heuristicを置かない。userlandだけでは修正不能なため事前許可対象である。
3. **generic weak-wait subscription**: native endpointのstrong capをpersistent epoll watchが持つと、kernel objectの最後refが減らずpipe EOF/EPIPE等が壊れる (`kernel/src/state/fd.zig:277-317,1020-1038`; `kernel/src/state/pipe.zig:192-199,232-245`)。userlandはcapabilityのstrong refをweakに変換できず、ABAなしのreadiness/final-releaseもkernel object layerだけがpublishできる。Step 3で`FD_FCNTL` commandを意味順の`GET_FLAGS=1, SET_FLAGS=2, SUBSCRIBE_WAIT=3, DUP=4`へ一括renumberする。current `DUP=3` (`kernel/abi/fd_abi.zig:107-110`) のcompatibilityは持たない。`SUBSCRIBE_WAIT(source, events, min_fd)`はWAIT/POLL/CLOSEだけのruntime-private fdを返し、kernel内でsource object/generationをretainしないweak back-linkにする。source readiness changeでsignal、source final releaseでHANGUP + detach、subscription closeでback-link removalを行う。syscall番号は増やさず、fcntl argument/returnのexact layoutはStep 3のABI redで固定する。これはpipe固有知識ではなく全waitable kernel object共通の非所有monitor primitiveであり、実装前にkernel editの許可を取る。
4. **channel peer-close semantics**: current channel は両 side 合計 `ref_count=2` だけを持ち、最後の全 ref まで queue を clear しない (`kernel/src/state/ipc.zig:257-294`)。M5.6R/M5.8Rは総refが1の間のpersistent HANGUP (`kernel/src/state/fd.zig:687-692`) とSIGKILL teardown時のwaiter wake (`kernel/src/syscall/process.zig:237-269,289-308`) までは入れたが、normal close、direct receive、normal exit、exec/CLOEXECに同じwake/queue cleanup契約はない。owner leaseには side ごとの最後の ref、dead peer 宛 send=`CLOSED`、remaining side poll=`HANGUP`、sleeping waiter wake、dead receiver queue の transferred caps 即解放が必要である。これは任意 userland brokerが安全に capabilityを保持するための kernel object lifetime であり、netd 固有機能ではない。既存status `CLOSED=6`を使える (`kernel/src/syscall/numbers.zig:88-95`) ためsyscall番号/layoutは変えず、side lifetime semanticsだけを完成させる。Step 14で全exit/close/exec経路のredを出してからkernel editの許可を取る。
5. **`PROCESS_EXEC_FROM` のatomic commit**: current実装はtarget metadata clone後にVMA replaceが失敗でき、その後もstaged process cleanup、CLOEXEC close、scheduler context installの順に不可逆変更を行い、最後の二処理もerrorを返し得る (`kernel/src/syscall/process.zig:894-930`; metadata/VMA mutationは`kernel/src/memory/user_vm.zig:232-241`, `kernel/src/state/vma.zig:727-750`)。さらにcontext installはcurrent threadだけを更新する (`kernel/src/scheduler_connection.zig:1947-1990`)。Step 19ではsame-principal sibling threadsをsource stateのままquiesceし、全allocation/validation、target address-space/native-fd/context planをpreflightする。failureなら全siblingsを旧sourceでresumeし、successならprocess/scheduler lock下の一回のno-fail commitでstaged address space、VMA table、native fd table、current contextをmoveし、siblingsをterminateする。old source state/staged shell cleanupはpost-commit no-fail finalizerへ渡す。address space、fd table、trap frame、sibling scheduler stateを同時に所有するkernelだけがこのatomicityを提供できるため正当であるが、syscall番号/manifest taxonomy/LPR kind知識は追加しない。staged-state adoptionを示すnamed flagを既存flags引数に意味順で定義し、old `flags=0` compatibilityは持たない。failure injectionとtwo-thread/in-flight syscall red後にkernel edit許可を取る。
6. **active stack thread exit**: redはM5.8Rで再現済みで、currentのmusl tail byte照合 + 静的4KiB stack + inline asmは暫定対処である (`lpr_dispatch.c:919-927`; `lpr_process/syscalls.c:20-26,170-210`; `refactor-plan.md:612`)。Step 1で実行中stack mappingを渡す独立redをmusl固有byte列に依存せず再固定し、事前許可後に既存`thread_exit` flagsへpost-switch-unmap bitを追加する。userlandは実行中stackの解放とthread exitをatomicにできないためkernel scheduler境界が正しい。currentは`CLEAR_TID` bit 0のみで (`kernel/abi/process_abi.zig:54-58`)、exit実装は`kernel/src/syscall/process.zig:933-963`にある。bit 0とsyscall番号は不変、旧flag compatibilityを加えず、置換後にbyte照合/static stack pathを削除する。

kernel に pathname、Wayland、SCM kind、daemon handle table を入れる案は明示的に却下する。

## 10. 段階的移行計画

各 Step は merge/受け入れ単位であり、途中 adapter は次 Step へ持ち越せても、Step 自体は既存 full regression green で終える。baselineはM5.8R完了の`71d0182`である。ただしMesa shader-cache/ext4 corruptionはfd-opsと無関係なデータ破壊なので、本Step列に混ぜずPhase 6最初の独立legで先行修正する (§14)。本セッションは文書改訂だけであり、以下を実行しない。

### Step 1 — signal / thread-exit boundary の完了

- M5.6Rでgreenのasync-entry alignment、poll/epoll wait-boundary delivery、publish-before-wakeをregressionとして維持し、残るrestorerの`sub $8,%rsp` (`lpr_entry.S:89-98`) をSysV call-alignment redで除く。
- process-directed signalのowner/event-loop thread契約を先にuserland+kernel unit/QEMU redへ固定する。ownerとworkerがともにblocking中でもSIGTERM handlerがownerへ一回だけ届き、owner exit/re-registerがgeneration-safe、Sway TERMが10秒以内に終了することを要求する。そのredと§9.4 item 1の理由を示してkernel事前許可を取る。
- 許可後、signal-control ABIを`REGISTER=1, DELIVER_PENDING_FRAME=2, RETURN=3`へ全consumer同一artifactで一括変更し、v2-style/旧sub-op shimを置かない。`REGISTER`にprocess owner generationを与え、schedulerの任意blocked-thread選択を削除する。
- M5.8Rで再現済みのactive-stack unmapは、musl tail byteを見ない専用pthread redへ固定する。§9.4 item 6の理由で別途kernel事前許可を取り、`thread_exit` post-switch-unmap flagを入れて、tail byte照合、静的4KiB stack、inline-asm exitの暫定pathを削除する。
- Step完了時にenduranceのTERM oracleをcurrent `kind=exit status=137 escalated=1`から`kind=exit status=0 escalated=0`へ反転し、escalation markerが0件であることを要求する。direct SIGKILLの`status=137 escalated=0`はこのStepでは変えない。

消えるclass: 任意workerへのprocess signal配送、musl固有byte列依存、active-stack unmap、restorer alignment。fd構造自体はまだ変えない。

### Step 2 — entry/OFD flags の一本化

- entry `fd_flags`を唯一のCLOEXEC source、object common access/statusを唯一のOFD flags sourceにする。
- current object rightsを`rights_ceiling`へ移し、既存entryを同じ値の`effective_rights`で初期化する。全operationはentry rightsを先、object/backend ceilingを次に検査し、dupはentry rightsをcopyする。v7 adapterはcurrent equal-rights subsetだけをround-tripする。
- payload `flags`、socket `cloexec`、legacy sync helperを削除し、filed handle側fd flagsは0/ignored adapterにする。
- common fcntl、dup/close_range、exec preserveはcommon fieldだけを見る。v7 serializerはcommon stateから旧layoutを生成する。

消えるclass: socket CLOEXEC手動同期、entry/object/payload flagsの食い違い、`MSG_CMSG_CLOEXEC`がOFDへ漏れること。

### Step 3 — typed native result/registry API

- `lpr_linux_fd_t`、`lpr_native_fd_t/ref_t`、result decode/adopt/get-info、fixed-endpoint registry、native policy registryを追加する。
- §9.4の事前許可後、native value/count-return ABIの全errorを`-status`へ一括正規化し、kernel、libipc、LPR、全service callerを同じartifactで更新する。status/new-fd domainは維持し、old positive-error/revents推測compatを削除する。同じflag-dayで`FD_FCNTL`を`GET_FLAGS=1, SET_FLAGS=2, SUBSCRIBE_WAIT=3, DUP=4`へ全renumberし、generic weak-wait subscriptionをtyped runtime-private refとして追加する。
- raw `<16`/`>=16`とfixed番号参照をnative moduleへ集約するが、各fd kindはこのStepではadapter経由で現挙動を保つ。
- host/kernel testでstatus/error各0〜6、I/O bytesとpoll ready count各0〜7、`F_GET_FLAGS`値1〜6、value型ioctl、first-dynamic 16、known/fixed fd、wrong kind/rights、double-adopt/closeを検査する。weak subscriptionはsourceをretainしないこと、readiness、source-final HANGUP、source/subscriber slot generation ABA、subscriber close/process deathのback-link回収をpipe/event/timerで検査する。`FD_GET_INFO`にはstable identityがないこともlayout testで固定し、raw infoから`ofd_key`を捏造するAPIを作らない。

消えるclass: syscall result、既知fd validity、logical type判定が同じliteral 16を共有すること。

### Step 4 — common backend/native refs へのfield移行

- filed/tty/DRM/input/socket/dmabufのhandleとDRM/input/socketのwait fdをobject common `backend/primary_native/wait_native`へ移す。
- kind payloadのgeneric mirrorを削除し、backend module内adapterだけがtyped common refを読む。ownership/borrow/closeをhost testで固定する。
- v7 serializer/restoreだけはnative module経由でlegacy fieldsへ変換し、correctness sourceにはしない。

消えるclass: handle/wait fdの二重帳簿、install/restore一枝だけの同期漏れ、raw wait fd二重close。

### Step 5 — logical/native namespace のcut

- pipe/event/timer/dmabuf/stdio/bootstrapをhidden native refを持つ明示objectへ移す。runtime-private wire/session/scratch refsもpolicy registryへ登録する。
- full processごとにshared OFD-state arenaを作り、local/native object constructorがcreation-time UUID/slot/generation keyを採番する。dupは同じkey、slot reuseはgeneration更新とし、external shareはStep 13までdisableする。
- logical allocatorからnative occupancyと`lpr_runtime_reserved_fd()`による240〜246除外 (`lpr_fd/control.c:632-675`) を除き、table miss native fallbackを削除する。
- logical 3〜15、240〜246、同じ数値のhidden native slot、arena slot ABA、fork/exec/close_range後のfixed endpoint、low-fd two-hopを検査する。

消えるclass: logical/native同番号衝突、low-fd再転送不能、logical closeが別namespaceを閉じること、runtime-private refの手動slot知識。

### Step 6 — ops/factory registry scaffold

- 全10 current kindsに全callbackを埋めたops recordを一個ずつ定義する。最初はbackend-local legacy adapterで、中央dispatchはまだ一括置換しない。
- linker/generated registry、deterministic priority、unique FILED fallback、duplicate wire id、NULL/capability矛盾のstartup validationを入れる。
- open resolverをpure match→一回openの`NOT_MINE/OPENED/CLAIMED_ERROR`化し、drmdがclaim後に`ENOENT`でもfiledへ落ちないpoison testを入れる。FILED open/fstat一回→DEVICE post-resolve adoptをrelative path/symlinkでも検査し、adopt error時はticket rollback・再open 0、ambiguous matchはfail closedにする。

消えるclass: kind registration/restore factoryの手動central list、M5.3c型のclaimed-error silent fallback。以後のStepはchainを一群ずつ消せる。

### Step 7 — data I/O dispatch

- read/write/readv/writev/pread/pwrite/lseekをpinned object→opsへ移す。
- `lpr_dispatch.c`のsocket入口特例を除き、pipe/socket vector atomicityとpartial-I/Oをbackend testで固定する。
- unsupported/poison opsがfiled/nativeへ落ちず明示errnoになることを検査する。

消えるclass: data I/Oの8枝chainとsocket二重dispatch、新kindのread/write片側追加漏れ。

### Step 8 — metadata/directory/mapping dispatch

- fcntl-specific/ioctl/stat/getdents/truncate/sync/mapをopsへ移し、common fcntlはcoreだけに残す。
- mmapのdmabuf/DRM/FILED chainをmap transactionへ変え、fd close後もmapping ticketがbackend refを保持することを検査する。
- metadata/mmap central kind chainとfiled/native fallbackを削除する。

消えるclass: fcntl/fstat/ioctl/mmapごとのkind再列挙、mapping存続refとfd refの混同。

### Step 9 — pinned refs と logical dup

- naked object pointer getterをgeneration付き`lpr_fd_get/put`へ置換し、全ops呼出しをtable unlock下にする。
- dup/F_DUPFD/F_DUPFD_CLOEXECは同一objectへのentry aliasだけを作り、backend DUP/native dupを呼ばない。
- close/reuseとin-flight I/O、same-OFD canonical map、fd table growthのrace testを入れる。

消えるclass: lock解放後pointer UAF、local dupでOFD分裂、table lock中backend RPC。

### Step 10 — close/dup2/close_range/normal-exit core

- no-fail close prepare、entry detach linearization、nonblocking pin-drain finalizer、idempotent confirm reaperを導入する。backendは当面同期adapterでもよい。
- dup2/dup3 target replacementを一回のtable commitにし、target close errorを成功結果へ漏らさない。
- close_rangeとnormal exitをunique object generic close-allへ移し、全kindでfinalizer一回を検査する。direct closeのdefinitive backend errorはerrnoを返しつつfdが再利用可能なこと、pin-drain/dup2/close_range/exitのdeferred errorはcaller結果を覆さずdiagnosticに一回記録されることをfault injectionする。

消えるclass: final close漏れ/二重close、normal exitのM5.8R socket専用pathを含む手動per-kind loopとTTY欠落、dup2 ABA/target half-close。

### Step 11 — poll/select wait planner

- poll/ppoll/select/pselectを`poll_prepare/native_wait_source/recheck`へ移し、native source identityでdedupe/fan-outしてmixed setも一回の`fd_wait_many`でarmする。logical occurrencesが256超でもunique sourceがnative table 256以内であることを検査する。
- wake後generation/signalを再検証し、event/timer deadlineを同じplanへ統合する。
- wait source未提供のserviceだけは明示compat 10ms adapterを残し、削除期限をStep 16に固定する。

消えるclass: native-only集合判定、poll readiness chainとwait-fd chainの二重更新、mixed setの不要な10ms polling。

### Step 12 — epoll integration

- registrationを`ofd_key + registration fd + generation`へ変え、`epoll_register`と`EPOLL_TARGET`でobjectごとに可否を決める。regular fileはpoll readyでもepoll `EPERM`となるpoison testを入れる。
- single-process weak-watch shell/subscriberを導入し、native targetはStep 3のweak-wait subscription、service targetはweak subscriber ticketをinterestが所有する。epoll waitをStep 11 plannerへ載せ、close/reuse/final strong-root通知とepoll自身のinterest cleanupをops化する。
- alias close、fd number reuse、edge/oneshot、nested graph、last pipe reader/writer後のEOF/EPIPE、timer expiry、source-final HANGUP/auto-remove、watcher SIGKILL後のsubscription回収を検査し、watch capだけがsourceを延命しないことをcount oracleにする。central allowlist/EPOLL special closeを削除し、cross-process shared graphはStep 17で有効化する。

消えるclass: epoll登録とwaitで別kind listを更新すること、file index再利用による別object通知、EPOLLだけのclose例外。

### Step 13 — service ABI v3 flag-day scaffold

- §9.3の理由でglobal ABI 2→3、request/reply magic末尾2→3、personality note/trap/runtime magic末尾2→3、INPUTD/NETD common ids、affected service op全renumber、common txn/lease request prefixを一括導入する。
- LPRS generic OFD-anchor op/tableをこのtaxonomyへ予約し、arena slot-scoped ticket + data-cap ordinal + shared status + stable generationを結ぶproof protocolをsynthetic pipeで検査する。LPRSがdata endpointをretainしないlast-reader/writer oracleも入れ、実pipe transferはStep 22まで有効化しない。
- global version consumerを全て更新し、koboxd ext4/storage data pathは変更しない。全serviceはまだv2相当動作をv3 adapterで提供する。
- envelopeを64 bytesのままにするか拡張するか、netd 64KiB page descriptorを含むexact id/opcode/all magic/header/payload offset表を先にlayout red testへ固定し、v2 shimを置かない。

消えるclass: serviceごとのprivate id/headerと、後続機能ごとのABI bump。lifecycle挙動はまだ切り替えない。

### Step 14 — kernel peer-close と owner-lease substrate

- currentのpersistent HANGUP/SIGKILL wakeはregressionとし、normal close、dupした片側ref一個のclose、side-last-ref、direct receive、normal exit、exec/CLOEXEC、sleeping poll、dead-receiver queued capsを独立redにする。最後のside refだけがpeer deathになり、peer send=`CLOSED`、poll=`HANGUP`、waiter wake、queue cap baseline回復となることを要求する。
- §9.4 item 4のredと理由を示して事前許可を取った後に、channel side-last-ref、CLOSED/HANGUP/wake、dead-receiver queue cap cleanupを汎用kernel semanticsとして入れる。
- userland共通lease/txn tableをsynthetic backendで検査し、実serviceは次Stepから一群ずつ移す。

消えるclass: PID/queued HUP/`notify_pending`をowner livenessとして推測する必要。kernelにLPR kind知識は入れない。

### Step 15 — filed/termd lifecycle migration

- filedとtermdのowner attachment、unique object close、prepared ticketをv3 lease transactionへ移す。
- local dupからservice DUPを除き、normal/SIGKILL、partial prepare、confirm retry、capacity回収を検査する。
- filed OFD status/offsetとtermd session/job-control data-planeは既存の正を維持する。
- filed/termd OFDのnon-watch strong rootとepoll weak subscriber通知をservice共通lifecycleへ載せる。
- current FILED_MEMFD SCM adapter経由のsender/receiver handleもowner leaseで数え、Swayのnormal/TERM/direct SIGKILLの毎回にfiled active handles/sessionが開始baselineへ戻ることを検査する。このStepでrunnerの`baseline + 4 * forced-rounds`許容 (`tests/run-lpr-qemu-sway-endurance-smoke.sh:86-108`) を削除し、シグネチャ4名のいずれかが1個でも残ればfailに反転する。

消えるclass: filed/tty handle close/fork用手動ref、client death時だけ残るsession/page、confirm timeout後の二重処理。

### Step 16 — netd/drmd/inputd lifecycle とwait endpoint

- netd/drmd/inputdのowner attachment/notify refをv3 leaseへ移し、owner refとqueue refを別に数える。
- netdの`reap_orphaned_sockets()` open-time scanを削除し、lease peer-closeが直接socket/notify/queueを回収することを「新規openなし」で検査する。`notify_pending`はcoalescingのみに残す。
- 三serviceのnon-watch strong rootとepoll weak subscriber/final invalidationを共通moduleへ載せる。
- ttyを含むblocking serviceにreceiver-specific wait endpointを揃え、10ms adapterを削除する。
- normal/SIGKILL、32 socket endpoints、DRM/input wait attachment、notify coalescingを同一VMで回収確認する。完了時にfixtureの2/4/7/9の強制終了4回capを外し、direct SIGKILLを20回連続実行する。毎回filed/drmd/netdの全handle/session/socketがbaselineへ戻り、drmdのFB/dumbもbaseline、`DRMD_HANDLE_MAX=32`の残容量が不変であることを要求する。

消えるclass: `notify_pending` alive誤判定、service wait fd共有によるwake取り合い、crash時socket/notify/DRM/input handle leak。

### Step 17 — local/native/runtime-private fork transaction

- generic fork ledgerを導入し、pipe/event/timer/epoll/mappingとnative policy registryをprepare→clone→両側confirm/rollbackへ載せる。
- creation-time arena VMOを親子で共有し、EPOLL shared graph + mutation notifyを有効化する。parent ADD→child wait、child MOD/DEL→parent、parent exec後もchild graph継続、別processの最後のstrong ref closeで両waiter wakeを検査する。
- runtime-private wire/page/session refsをpolicyどおりchild drop/recreateし、手動service reset listを削除する。
- parent/child confirm順序、各点SIGKILL、kernel clone failureをfault injectionする。

消えるclass: event/epoll COW分裂、scratch/session共有破壊、fork childだけの個別native cleanup。

### Step 18 — service-backed fork transaction

- filed/termd/netd/drmd/inputd objectをunique OFDごとにclone前prepareし、childはmaterialized attachmentをadoptするだけにする。
- child post-fork kind chainとservice別top-level cancel sequenceを削除する。
- parent-close race、child-before/after-confirm kill、全backend prepare failureを検査する。

消えるclass: fork後の共有handle破壊とbackend追加ごとのrollback漏れ。attempt1のfork確定根因はここで構造的に閉じる。

### Step 19 — exec ops/coordinator を v7 adapter 上で導入

- 全opsのexec prepare/encode/decode/target-confirm/source-rollbackを有効にし、source tableをkernel commitまで不変にする。
- v7 framing adapterを一時使用しつつtarget service/native refsを先にmaterializeし、inherited socket source-close→EBADFを止める。
- §9.4の事前許可とfailure-injection red後、kernel `PROCESS_EXEC_FROM`をsibling quiesce、staged address-space/native-fd/context preflight、一回のno-fail visible commitへ変える。error時は全threadsが旧stateで再開、success時はcaller以外terminateをtwo-thread/in-flight syscallで検査する。
- staged-state adoptionは既存flags引数のnamed flagとし、old `flags=0`をrejectするABI layout testを先に固定する。sealed target-state VMOがsource address-space交換後もtarget mappingとして残り、new runtimeがallocationなしでbuilder slotsをadoptするoracleを入れる。
- prepare/PROCESS_EXEC_FROM failureでsource継続、target death/confirm retryを検査し、central kind serialize/close chainを削除する。
- `M56_SOCKET_FORK_EXEC_OFD_OK cloexec=1 shared_flags=1 status=0`をexact oracleにし、socket source/target refの順序を固定する (`attempt2:812-823`)。

消えるclass: exec commit前の旧table破壊、preserved targetをsource final closeで無効化、service別exec reserve。v7のalias/EPOLL制約は次Stepで消す。

### Step 20 — exec v8 manifest flag-day

- fd-table 7→8とimage ABI 10→11を一回で切り替え、entry/object/blob/cap ordinal、two-pass graph restoreを導入する。
- non-CLOEXEC aliases/effective rights、same OFD key、arena/shared EPOLL ticket、service/native refsを一回だけatomic restoreする。same-lineageのarena capとunrelated targetのslot-scoped proofを取り違えず、entry 0/weak-edge-only recordはstrong attachmentを作らない。manifest bundleのcap数0/1/16/17/19/20/target-free-slots/+1とmid-chunk deathを検査する。
- filed structural validationへ変え、v7 reader/writer/old taxonomyを削除する。decode各点のfailureでpartial publish 0を検査する。

消えるclass: dup alias/OFD共有消失、EPOLL強制drop、raw native slot wire、kind別restore更新漏れ。

### Step 21 — generic ancillary broker と既存 INPUT/DRM/FILED_MEMFD 移行

- opaque unique-object capsule + per-occurrence rights、delivery anchor、sealed bundle chain、253-fd transaction、recv consume/PEEK、全層capacity prefix/CTRUNC規則をnetd/LPRへ入れる。
- current INPUT/DRM/FILED_MEMFD transferをops callbackへ移し、netd wireから`fd_kind/fd_flags/fd_handle/fd_aux`と`NETD_FD_KIND_FILED_MEMFD`を除く。FILEDの完成したmemfd semanticsはStep 23までenableしないが、M5固有wire kindはこのStepでopaque capsule adapterへ置換する。
- zero-byte/partial stream sendでancillary 0/1回、ordinary prefix→batch A→batch BでA停止、plain read discard、repeated PEEK、`[A,A,B]` alias途中CTRUNC、異なるentry rights、19/20、253 unique/same-fd occurrences、254=`EINVAL`、cap-heavy/near-full native table、bundle chunk中death、unknown proof、lock-order faultを検査する。

消えるclass: netd/LPR双方のtransfer-kind switch、stream ancillary順序ずれ、EMFILE data再配送、page/queue lock中filed RPC。attempt1のlock inversion classはここで閉じる。

### Step 22 — pipe/socket transfer とcycle GC

- pipe OFD anchor/capability importとsocket OFD reservation/receiver wait attachmentをtransfer opsへ追加する。
- netdの全nonqueued strong roots/queue dependency edgesのmark-sweepを入れ、self-send、A↔B cycle、queued-only SCC、owner SIGKILL後のcapacityを検査する。
- separate sendmsgで同じOFDを二回・異なるattenuated rightsで受信してcanonical objectへmergeし、entry rightsは分離したままoffset/status/epoll identityを共有する。last reader/writer closeのEOF/EPIPE、SCM先だけが最後のstrong refを持つ間のepoll保持とclose時final wakeも検査する。

消えるclass: logical番号をnative capとして運ぶこと、socket queue cycle leak、別transfer間の同一OFD分裂。

### Step 23 — filed memfd/VMO transfer

- filed opsのmemfd export/import、canonical VMO、shared OFD offset/status/seals、rights/proof validationを実装する。`VMO_FILE` kindは作らない。
- 196,608 bytes、first/last pixel、MAP_SHARED双方向、readonly、seals、separate/batch alias、3〜15二hop、CLOEXEC、253境界を検査する。加えてM5.7Rで未検証の、SCM_RIGHTS receiverでのcross-process `pread`と`MAP_PRIVATE`が`EAGAIN`になるredを固定する。`pread`はshared filed OFD owner、`MAP_PRIVATE`はStep 8のmap transaction/mapping ticketがreceiver leaseの正しいbackend refを保持することで閉じ、fd close後もprivate mappingのread/COWが継続することを要求する。Step 8単体はこのcross-process契約を完了したことにしない。
- sender/receiver/queued SIGKILLを各20回（queuedは`imported=0`）、filed `transfer_capacity=64 map_bytes=4096`、netd endpoint capacity 32の再確保を同一VMでbaselineへ戻す。

消えるclass: filed memfd横断特別kind、metadata copyだけのOFD分裂、low-fd retransfer不能、kill時VMO/ticket leak。旧M5.6機能要件はgeneric opの一実装として成立する。

### Step 24 — 実 wl_shm + Sway helper lifecycle と最終削除

- 実Wayland clientでglobals、196,608-byte XRGB8888 memfd、xdg configure/ack、attach/damage/commit/frame callbackを通し、1024x768 screendump中央8x8を厳密`#336699`判定する。
- transfer単体はpixmanを使ってよいが、real card0 + llvmpipe `sway-first-frame`も別にgreen維持する。pixmanだけでSway完了にしない。
- `socketpair(SOCK_STREAM | SOCK_CLOEXEC)`を実経路で検査し、両entry CLOEXEC、dup/exec規則を確認する。`swaybg_command -`と`xwayland disable`は維持する。Phase 6先行ext4 leg完了後は`MESA_SHADER_CACHE_DISABLE=true` (`tests/fixtures/sway_endurance.sh:23`) を外し、shader-cache有効の各23回とpost-run fsck cleanを要求する。
- M5.7RのSway限定`shm_open` preload (`tests/fixtures/sway_endurance.sh:39-45`; launcher適用は `lpr_sway_launcher.c:178-185`) を削除し、wlroots anonymous-file生成をbuild-timeの`memfd_create`経路へ恒久化する。これはStep 23のfiled memfd transferを使い、keymapのためにregular-file transfer全体を有効化しない。
- lifecycleはnormal 2回（最後のrecoveryを含む）、SIGTERM 1回、direct SIGKILL 20回の合計23回とする。全回clientは`status=0`、normal/TERM Swayは`kind=exit status=0 escalated=0`、direct KILLはcurrent LPR wait表現の`kind=exit status=137 escalated=0`にexact固定し、seatd正常終了、`orphan=0, stale=0, waitpid=1`を要求する。実測なしに124/143や他statusをsuccessへ正規化しない。各回filed/drmd/netd全handle/session/socketがbaselineに戻り、`+4`許容や強制終了4回capはない。iteration 23をnormal recoveryにする。
- 全回`M56_WL_SHM_TRANSFER bytes=196608`と`M58_WL_SURFACE_READY ... color=#336699 size=256x192`及びpixelをexact countする。iteration 1とrecovery 23でQMP固定入力を送り、`KEY_A 30/1/0`、REL `+7,-4`、`BTN_LEFT 272/1/0`の`M58_INPUT_PASS`相当を要求する (`tests/run-lpr-qemu-sway-endurance-smoke.sh:29-32`)。これがM5.7R/M5.8Rで旧構造上greenだった入力の、新構造上での再達成である。
- fake PRIME fence transportは本Step外なので`LP_NUM_THREADS=0` (`lpr_sway_launcher.c:178-181`) はここで外さない。最後にcompat adapter、central kind branch、payload mirror、raw fd boundary、v7、service別lifecycle entrypointを0にする。

消えるclass: 移行専用dual path、helper orphan/stale socket、実workloadだけのqueue/exec/kill leak。旧M5.6と旧M5.8受け入れを新構造上で再達成する。

## 11. 各 Step の regression gate

各 Step は変更箇所だけの unit testで終えず、少なくとも次の共通 gateを同じ最終 artifactで通す。実装セッションでは runner の `.artifacts` を jobごとに隔離し、別検証jobのartifactを再利用・上書きしない。

Host/static gate:

- `tests/run-lpr-fd-table-tests.sh`
- `tests/run-userland-service-abi-layout.sh`
- filed VFS/cache tests、termd pgrp signal unit、pack `go test ./...`
- kernel unit tests。EFI buildは`cd kernel && zig build efi`とし、repo rootで`zig build-obj`を実行しない。C compileが必要な時は`zig cc`で代用せず、repo規律どおりsandbox外のWSL clangを使う。
- `git diff --check` と、Step固有の no-legacy static scan。

QEMU/full gate:

- fd-pipe、pipe-stress、epoll、async-signal、shared-mapping、state-leak、pty teardown、GNU coreutils、clang cold/endurance。
- evdev、libinput+seatd、idle uevent monitor。
- drm card0/prime/page-flip/restart、kms modeset、Mesa inventory。
- Sway inventory、real first-frame、socket repeat。
- 基準化後の M5.6b ext4 sync/unlink journal smokeとpost-run fsck。

M5.8R current enduranceは移行前redとして、forced exit 4回cap、filed `+4/回`、TERM `escalated=1`を一時的に厳密許容する。ただし許容は、Step 1でTERM escalation、Step 15でfiled `+4`、Step 16でforced-exit capの順に必ず反転する。Step 16以降はSIGKILL 20回連続の毎回filed/drmd/netd baseline回復を共通gateにし、Step 23はSCM sender/receiver/queue別各20回、Step 24は画面+入力+合計23回の実workloadを上乗せする。一度反転したoracleを後のStepで元の許容値へ戻さない。

Step固有 testが新しい redを示す間、既存oracle、iteration数、timeout、capacityを緩めてgreen扱いしない。Step完了時は open object、native refs、service handles、transaction tickets、queue caps、mapping数が開始baselineへ戻ることも検査する。

最終 static invariant の例:

- `LPR_FD_TABLE_KIND_` の参照は移行compatを除き ops定義/registry testだけ。
- `fd < 16` / `fd >= 16` は native ABI/result moduleだけ。
- payload に generic `flags`, `cloexec`, `handle`, `native_wait_fd`, `active` mirrorがない。
- `exec.c`, `metadata.c`, `dup_pipe.c`, `lpr_socket.c`, `lpr_epoll.c`, `lpr_dispatch.c` に fd kind/active-helper dispatchがない。
- netd wire/queueに LPR kind-specific enumがない。

## 12. 未決事項

以下は実装で先取りせず、該当 Step の red test/measurement後に決める。

1. ops registry の linker section start/stop symbolを現 LPR shared-object linkで使うか、macroからgenerated registryにするか。manual central listは選択肢にしない。
2. local/native objectの cross-process OFD status storage。値とbackend stateを二重に持たないこと、fork/SCM後の `F_SETFL`共有を満たすことは決定済みで、VMO/shared service ticket等の表現だけが未決。
3. SCM_RIGHTS 一回の最大fd数。kernel cap上限19、reply/page caps、data framingを測り、明示上限かbatchかを決める。
4. Phase 6で filed regular file、eventfd/timerfd、dmabuf、tty、epoll/deviceのtransferをどこまでenableするか。memfd、既存INPUT/DRM、pipe/socket以外はtestなしにenableしない。
5. v8 header/recordのexact byte size/alignment/capacity。版数方針とentry/object分離は決定済み。
6. service transaction ticket/owner tableのcapacity、lease timeoutの有無、GC scan cadence。正常時にwall-clock timeoutだけでownershipを破棄しない。
7. active-stack unmap kernel flagのexact bit/argument layout。採用とStep 1での置換はM5.8R redにより決定済みだが、musl byte signatureに依存しないredと§9.4の事前許可後にexact layoutを固定する。
8. service ABI v3の各service別exact opcode/struct offset。意味group順の全renumber、version 3、一括cutは決定済みで、Step 13冒頭のlayout red testで数値表を固定してから実装する。

## 13. Phase 番号の確定

旧呼称`M6.0 = fd-ops`と`M7.x`は廃止し、以下を正式な番号とする。

| Phase | 内容 | 受け入れ境界 |
|---|---|---|
| Phase 6 序盤 | Mesa shader-cache/ext4 corruptionの独立修正を先行し、その後に本書Step 1〜24でfd ops / namespace / lifecycle / exec v8 / generic transferを完成 | shader cache有効のpost-run fsck clean。最終Stepで画面、input、lifecycle、owner baselineを新構造上で再達成 |
| Phase 6 後半 | §14の独立送り項目を依存順に閉じ、その後にSMP本格対応 | fd-opsで古いlifecycle/wait分岐を除去する前にSMPの並行raceを重ねない |
| Phase M6.1 | Sway起動、frame、mouse/keyboard latencyの実測と高速化 | Phase 6/SMP後のbaseline比較、threaded llvmpipeを含む |
| Phase M6.2 | Sway実用gapの解消、virtio-tablet絶対座標、seatd `EVIOCREVOKE`警告等 | grab不要の絶対pointerと実用workload |
| Phase M6.3 | Waylandアプリ導入 | foot/GTK demo/選定アプリと通常利用フロー |

入力配管自体はM5.7R/M5.8R (`d104007`, `00e8014`) で旧構造上のend-to-endを達成済みである。従って旧「M6.1 = 入力完成」を独立Phaseにせず、Step 24の`KEY_A 30/1/0`, REL `+7,-4`, `BTN_LEFT 272/1/0`で新構造上の再達成を証明する。Phase M6.1はそのgreen baselineの性能改善から始める。

## 14. M5 「Phase 6 送りメモ」対応表

`pacha_docs/refactor-plan.md:620` のslash区切り16項目を、本書のStepまたは独立legへ漏れなく対応付ける。「独立」の項目はfd-opsのStep番号を付けて混ぜない。

| # | 送りメモ項目 | 位置付け | 完了条件/境界 |
|---:|---|---|---|
| 1 | fd-ops vtable統一 | Steps 2〜12, 17〜20, 24 | common fields→ops dispatch→lifecycle/execの順にcentral kind chainを0へする。 |
| 2 | generic transferable-FD、A→B→C再転送 | Steps 13, 21〜23 | opaque broker、OFD canonicalization、low-fd two-hop、kill capacityを同時にgreenにする。 |
| 3 | native fd窓 (`fd < 16`) の形式化 | Steps 3〜5 | typed result/native refとlogical namespace cut。3〜15 two-hopで完了。 |
| 4 | opcode/ABI整理 | Step 1 signal sub-op、Step 3 native fcntl/return、Step 13 service v3、Step 20 exec v8 | 各flag-dayで意味順に全renumberし、v2/v7/旧sub-op shimを置かない。kernel対象は§9.4の事前許可が必要。 |
| 5 | netd backend ref一般化、pathname owner lease | Steps 14, 16, 21〜22 | `reap_orphaned_sockets()`をStep 16でlease-driven cleanupへ置換し、queue/transfer cycleはStep 22で閉じる。 |
| 6 | filed handle/session owner lease・transfer ownership | Steps 15, 21, 23 | Step 15で`+4/回`を0へ反転、Step 23でsender/receiver/queue各20 killとmappingを固定。 |
| 7 | drmd handle/session owner lease・transfer ownership | Step 16、transfer部はSteps 21〜23の共通枠 | `+5 handles / FB +2 / dumb +4`を0へ反転し、32-slot圧迫なし。fake PRIME fenceは#13の独立leg。 |
| 8 | process-wide signalのevent-loop thread配送 | Step 1 | current TERM `escalated=1`をowner-thread deliveryの`exit 0 / escalated=0`へ反転。§9.4 item 1のkernel事前許可対象。 |
| 9 | SCM受信filed memfdの`pread`/`MAP_PRIVATE` `EAGAIN` | Step 8 + Step 23 | Step 8のmap transactionを前提に、Step 23のcross-process owner/mapping redで完了。Step 8単体では完了扱いしない。 |
| 10 | wlroots keymap anonymous-file恒久化 | Steps 23〜24 | filed memfd transferを使うwlroots build-time `memfd_create`へ移し、Sway限定`shm_open` preloadを削除。regular-file transferをこのために広げない。 |
| 11 | nested epoll native wake統合 | Step 11のplannerが前提、Step 12で削除 | M5.7Rのnon-native target 10ms quantumはStep 11だけでは残り、EPOLL object/interestをplannerへ載せるStep 12完了時に消える。 |
| 12 | file VMO cacheのworking-set budget/DMA fragmentation | fd-ops外のPhase 6独立leg | 現256-slotを根拠なく固定せず、byte/slot/working-setを計測可能にする。fd lifecycleのcorrectness fieldにしない。 |
| 13 | fake PRIME fence transport、`LP_NUM_THREADS=0`解除 | fd-ops外のPhase 6後半独立leg、Phase M6.1より前 | explicit/implicit fence transportをDRM/VMO transactionで固定した後だけ`LP_NUM_THREADS=0`を外し、threaded llvmpipeを回帰。 |
| 14 | `/dev/shm` tmpfs | fd-ops外のPhase 6 userland/filed独立leg | kernelにpath知識を追加せず、filed mount/tmpfs semanticsと専用testで完了。 |
| 15 | native channel close/wake semantics統一 | §9.4 item 4 / Step 14 | current M5のpersistent HANGUP + SIGKILL wakeだけでは未完。normal close、direct recv、normal exit、exec/CLOEXEC、dead queue capsまでside-awareに統一。kernel事前許可対象。 |
| 16 | Mesa shader-cacheでext4 multiply-claimed block/directory corruption | **fd-ops外、Phase 6最初の独立leg** | `refactor-plan.md:600`のデータ破壊でありStep列に混ぜない。shader cache有効の再現red、修正、post-run fsck cleanを得てからStep 1へ進む。kernel修正に証拠が収束する場合は別件として事前許可を取る。 |

`refactor-plan.md:618` のvirtio-tablet絶対座標とseatd `EVIOCREVOKE`警告はslash区切り16項目の外だが、行方を失わないようPhase M6.2に置く。同行末の旧`M6.0 SMP`は本書§13に従い、Phase 6のfd-ops/独立負債の後半へ付け替える。
