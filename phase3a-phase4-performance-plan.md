# Phase 3A / Phase 4 — OS・GUI性能とSMP活用

## 1. 目的

Phase 4を起動・描画・入力だけの局所最適化にせず、GUIのcritical pathに入るOS基盤全体の性能phaseとする。

対象:

- in-kernel schedulerとSMP locality
- physical allocator、page fault、COW
- address space、TLB shootdown
- IPC、wait、wake
- filed、file-backed VMO、page cache
- DRM、Mesa、fence、page flip
- inputd、evdev、libinput、wlroots
- direct Sway session、Foot、GTK3 demo

network、USB、toolchainは、それ自体が測定上のGUI bottleneckでない限りPhase 4へ含めない。

Phase 3は次の二段に分ける。

- **Phase 3A**: Phase 4と並行し、起動時のkeyboard、relative mouse、absolute tabletまで完成させる。
- **Phase 3B**: Phase 4完了後、runtime hotplug/remove、USB HIDまで完成させる。

Phase 4を主作業とし、Phase 3Aはscheduler、VM、allocatorの調査を止めない独立commitで進める。

## 2. 実行原則

- 各Stepを独立commitにする。複数Stepを一つのcommitへ混ぜない。
- 親agentが測定、根本原因調査、kernel設計、統合を担当する。
- child agentは最大3本、互いに重ならない短い調査または小patchだけを担当する。
- 既存の固定挙動testを新設計のoracleにしない。
- 開発中は変更箇所のunit/invariant checkと5–10秒の対象scenarioを一回だけ実行する。
- 10周endurance、長時間resource確認、総合visual確認はPhase 5へ送る。
- QEMUは`.artifacts/bin/pacgo`で起動し、確認後は必ず停止する。
- kernel buildは`cd kernel && zig build efi`、C buildはWSL clangを使用する。nixとzig ccは使わない。
- kernel変更前に、対象file、red、守るinvariant、userlandで直せない理由を提示して一度承認を得る。
- EDID、write-cache、XDG iconなどの非致命的警告は、性能原因と証明されない限り触らない。

## 3. Step 0 — 性能契約と短時間benchmark

標準scenarioを次に固定する。

1. TTYから`/usr/bin/sway`を直接起動する。
2. Wayland socket、最初のoutput commit、page flip完了を記録する。
3. Footを起動し、map、PTY入力、resize、終了を記録する。
4. GTK3 demoを起動し、map、resize、keyboard/pointer入力、終了を記録する。
5. 短いWayland animationを5–10秒動かし、frame intervalを記録する。
6. GUI idleを5秒観測し、poll、wake、CPU idleを記録する。

rootfsへAlpine `gtk+3.0`と`gtk+3.0-demo`を追加する。`GDK_BACKEND=wayland`を使い、launcher、preload、Xwaylandへ依存しない。

4 CPUを通常構成とする。1 CPUはSMP効果を比較する診断profileに限定し、成功fallbackにはしない。

記録対象:

- cold / warm Sway startup
- Foot / GTK3 map time
- frame intervalとpresentation feedback
- IRQからWayland clientまでのinput latency
- CPU idle、scheduler wake、migration、steal
- allocator lock wait、page fault、COW
- TLB shootdown対象CPU数とACK時間
- IPC wait / wake、file cache hit / miss
- DRM submit、fence、page flip

traceはringまたはhistogramへ集計し、hot pathからserialへ出力しない。scenario終了時だけ要約をdumpする。permanentな性能専用kernel ABIは追加しない。

最初の測定で現在値、条件、初期目標を本文書へ追記する。目標の緩和・厳格化は許容するが、理由とbefore/afterを同じcommitへ残す。

### Step 0 baseline — 2026-07-17

条件はKVM、2 GiB、virtio-gpu、direct `/usr/bin/sway`、Xwaylandなし、5秒の640x480 animationである。Foot、GTK3、animationへQMPからkeyboard/pointerを入力した。各CPU数は独立bootを一回だけ完走させた。

| metric | 4 CPU | 1 CPU診断 | 初期目標 |
|---|---:|---:|---:|
| cold Wayland socket | 27.0 s | 25.0 s | 観測値 |
| cold Sway IPC | 39.0 s | 36.0 s | 観測値 |
| Foot map | 15.0 s | 10.0 s | 観測値 |
| GTK3 map | 17.0 s | 11.0 s | 観測値 |
| warm Sway IPC | 35.0 s | 30.0 s | 9.9 s以下 |
| animation throughput | 4.087 FPS | 7.695 FPS | 4 CPUが1 CPU比+20%以上 |
| frame p99 / max | 245 / 252 ms | 133 / 133 ms | 50 / 100 ms以下 |
| input p99 / max | 323 / 422 ms | 266 / 312 ms | 32 / 100 ms以下 |
| libinput lag warning | あり | あり | なし |
| Wayland Broken pipe | なし | なし | なし |

1 CPU値はring-only化直前の最後の完走値であり、比較値はprovisionalとする。保存値では4 CPU throughputが1 CPUより46.9%低く、複数runでも同じ方向を再現した。1 CPUはfallbackにせず、4 CPUのscheduler、wake、locality、llvmpipe並列度をPhase 4の主redとする。

手動確認ではGTK3 demo、別Run window、keyboard/pointer操作が動作し、Fishbowlは約10 FPSだった。体感は操作可能だがもっさりしていた。これは自動animationとはworkloadが違うため参考値とする。

`pacha_trace_emit()`はring記録だけにし、hot pathのserial出力を廃止した。animationとinputはscenario終了時にpercentileを一度だけ出力する。permanentな性能ABIは追加していない。

局所計測の判定に使えるよう、pacgoがvirtio-console各行をhost monotonic基準のµs値で別logへ記録する。guest realtimeは1秒分解能のため判定に使わない。IPC失敗後の再確認間隔は2秒から100msへ縮め、P3A短縮scenarioにもSway exec、socket、IPC markerを追加した。

CPU idle、scheduler wake/migration/steal、allocator lock、fault/COW、TLB、IPC/file cache、DRM fence/page flipのcurrent値は未instrumentedをredとする。kernel変更の承認前に値を捏造せず、各owner Stepの変更前に内部ring/histogramを追加してbefore値を残す。

GTK3にはMIME/pixbuf cacheと`fallocate`の`EOPNOTSUPP` fallbackが必要だった。blocking PTYの空readがEOFになるtermd shimの問題は別の短いuserland follow-upとし、標準scenarioは実際に動くinteractive Bash PTY入力を使う。目標値は変更していない。

## 4. Step 1 — schedulerdと外部policy ABIの全廃

現行bootで使用されていない`schedulerd`とexternal policy経路を削除し、in-kernel verified EEVDFへ一本化する。

- `userland/schedulerd`、build設定、artifactを削除する。
- `schedctl`、`sched_event`、external event queue、commit ioctlを削除する。
- `policyActive`とexternal-policy専用分岐を削除する。
- FD kind 15 / 16を削除し、`pipe`を17から15へ詰める。
- kernel/userland ABIを同時に切り替え、互換層を残さない。
- AP dispatch、thread context、signal、block/wake、verified scheduler coreは残す。

ABI変更理由は「producerもconsumerもない旧daemon境界を消し、既に実働中のkernel schedulerを唯一の所有者にするため」とする。

完了記録: 現行bootにscheduler FD生成・policy attach・daemon起動のcallerがないことを確認し、daemon、FD kind 15 / 16、event queue、commit ioctl、external-policy分岐を一括削除した。`pipe`はkernel/userlandともkind 15へ切り替えた。4 CPUの短時間bootでTTY、native pipe、外部process生成を確認した。

## 5. Step 2 — Phase 3A: 可変input registry

固定`INPUTD_DEVICE_COUNT=2`とevent番号によるrole判定を廃止する。

- input boot configをsize付きheaderと可変長device/module tableへ変更する。
- device recordへfdとstable PCI identityを格納する。
- seed0bootは全virtio-input deviceを渡し、列挙順を意味として使わない。
- inputd registryはstable id、generation、capability、公開event番号を分離する。
- netd、sysfs、udev publicationからevent0/1と固定PCI pathを除去する。
- LPR parserをmulti-digit event nodeへ対応させる。
- producerとconsumerを同じcommitで切り替え、旧boot ABIを残さない。

runtime add/removeはまだ実装しない。startup時のdevice数、role、順序だけを可変にする。

## 6. Step 3 — Phase 3A: event-driven input

- 可変IRQ fdをgeneric wait setへ登録する。
- readyになったdeviceだけをdrainする。
- 全IRQ poll、全handle notify、全lease reapの通常時全件走査を除去する。
- SYN_REPORT単位でeventを公開する。
- emptyからreadableへ変わる時だけclientを通知する。
- IRQ readiness、evdev publish、Wayland client受信を同じmonotonic clockで対応付ける。
- pacgoへkeyboard+mouse、keyboard+tablet、device順序反転profileを追加する。

Phase 3A完了条件:

- raw evdev capabilityとSYN_REPORTが正しい。
- libinputがkeyboard、relative mouse、absolute tabletを正しく分類する。
- Swayで三種類の入力を操作できる。
- device順序を変えてもroleが変化しない。
- idle時にinputdの周期pollと全件走査がない。

## 7. Step 4 — scalable in-kernel scheduler

CPUごと256 entity固定配列、runqueue全copy、global scheduler lockをruntime経路から除去する。

- thread生成時にstable scheduler entityを確保する。
- per-CPU runnable setをintrusive augmented treeで管理する。
- verified coreはweight、vruntime、eligibility、deadline、state transitionだけを担当する。
- kernel adapterがdynamic chunk、tree、CPU ownershipを担当する。
- wake/IRQ経路ではallocationしない。
- per-CPU lockへ分割し、migration時はCPU番号順に二つのlockを取得する。
- scheduler容量をdynamic thread tableへ追従させ、通常負荷で`FULL`を返さない。

守るinvariant:

- 一つのthread generationは一つのownership stateだけを持つ。
- 同じthreadを複数CPUが同時実行しない。
- blocked / dead threadをrunqueueへ残さない。
- generationの古いwake、stop、commitを拒否する。

完了記録: kernel runtimeからCPUごとの256 entity配列、runqueue scratch copy、`pacha_kernel_sched` link、単一verified lockを除去した。thread slotは生成時に非移動scheduler nodeを持ち、thread table拡張時は追加範囲だけをstable chunkから確保する。各CPUは独立lockとintrusive augmented treapを持ち、deadline順探索をsubtree minimum eligibility / vruntimeでpruneする。wake、block、pick、charge、exit、handoffはallocationなしのscalar EEVDF transitionへ切り替え、migrationはCPU番号順の二重lockで一括更新する。generation tokenをpick、timer、handoff、futex waiterへ通し、stale transitionをcommitしない。CPU 0上のthread exitでlocal successorがない状態をprocess exitと誤認していたSMP lifecycle bugも、interruptible idle待機へ修正した。C scalar lifecycle test、300 runnable entityのtree invariant、kernel unit build、4 CPUのstatic / dynamic pthread短時間scenarioを通し、QEMU停止と`sched-core` faultなしを確認した。性能値はStep 5のone-shot timer / locality測定前なので、このStepでは更新していない。

## 8. Step 5 — scheduler fast pathとSMP locality

- periodic tickごとのparkと再選択を廃止する。
- per-CPU one-shot timerでslice期限を管理する。
- eligibleな競合threadがなければcurrent threadを継続する。
- wake時はlast CPUを優先し、runqueue差が1を超えた場合だけ再配置する。
- idle CPUは最も混雑したCPUからrunnable entityを一つstealする。
- pending wakeがあるCPUへ重複IPIを送らない。
- llvmpipe workerが4 CPUで同時進行していることを測定する。

完了記録: LAPICをper-CPU one-shotへ切り替え、BSPの1 ms timekeepingだけをsoftware rearmし、idle APではtimerを停止した。4 ms slice満了時もeligibleな競合がなければrunning ownershipを維持し、park/reinsert/context copyを行わない。wakeはlast CPUを優先し、runqueue差が1を超えた時だけCPU番号順dual lockで移す。旧CPUがblock tailを実行中のgenerationは移動せず、同時実行を防ぐ。idle APは最大8 nodeのbounded scanで最も混雑したcompatible runqueueから一つstealし、wake IPIはpending edgeを集約する。fresh 4 CPU direct Sway＋5秒animationは正常終了し、Step 0の4.087 FPS / frame p99 245 msから11.001 FPS / 95 msへ改善、maxは100 msだった。inputはp99 85 ms / max 120 msでStep 0より改善したが目標未達のためPhase 3/4へ残す。QMPが返す4本のvCPU thread-idを50 ms窓で76 sample採取し、最大同時進行4、union 4、CPU別progress 284/389/384/419 tickを確認した。hot path traceや性能専用kernel ABIは追加せず、QEMUは停止した。

## 9. Step 6 — physical allocator

allocator lock waitとfault時間がredであることを確認してから変更する。

- order-0 allocationへCPUごと64 pageのmagazineを追加する。
- 32 page単位でrefill / drainする。
- global線形extent arrayをzone-aware buddyへ置き換える。
- buddy metadataはboot時に物理メモリ量から確保し、runtime固定容量を持たない。
- normal pageとlow / DMA制約pageをzoneで分離する。
- contiguous allocationはbuddyの高orderを使う。
- page fault、COW、teardownを一つのglobal lockへ集中させない。

page数保存、二重free禁止、制約範囲、contiguous alignmentをinvariant testで確認する。

測定記録（2026-07-17）: 4 CPU、direct Sway、5秒animationのうちguest tick 30–40秒を、temporaryなlock/fault histogramで測定した。PMM lockは29,243 acquisition中6回だけcontended（0.021%）で、contended waitは平均2,879 cycles、最大8,473 cyclesだった。handled faultは8,294回（COW 374、lazy 7,920）、平均30,310 cycles、最大5,074,106 cyclesで、99%点は640,000 cycles未満のbucketだった。同じrunは11.119 FPS、frame p99 / max 92 / 93 msで完走した。

この条件ではallocator lock waitがredではなく、fault tailとも対応しなかった。page fault、COW、teardownを先に直列化しているglobal address-space lockの分割前にmagazine / buddyへ置換しても、GUI critical path上の改善根拠がない。このためStep 6の変更gateは未成立とし、temporary metricは全削除した。固定65,536 rangeとlinear extentはcorrectness / capacity debtとして残すが、Step 7でaddress-space lockingを分割した後に同じ10秒区間を再測定し、allocator contentionがredへ変化した場合に本Stepへ戻る。性能目標は変更しない。

## 10. Step 7 — VM / TLB

- address spaceごとにactive CPU maskとTLB generationを持たせる。
- PTE変更時はgenerationを進め、active CPUだけへshootdownする。
- 後からaddress spaceへ入るCPUはgenerationを比較し、staleならuser復帰前にlocal flushする。
- 32 page以下は`invlpg`、それ以上はlocal CR3 flushを使う。
- 一つのmapping操作の変更範囲を一回のshootdownへbatchする。
- remote ACK待ちは残し、timeoutや全CPU broadcastでcorrectnessを代用しない。
- address-space lock中のcopy、allocation、remote ACK待ちを可能な限りlock外へ移す。

進捗記録（2026-07-17、lock topology）: 全processを止める単一lockを廃止し、address-stableな`UserAddressSpace`ごとの再帰lockへ切り替えた。単一space操作は対象lockだけ、fork / exec / process-mapはprincipal index順のpair lock、VMO revokeは全spaceを昇順に取得する。fork後に共有されるNativeVmo / NativeCowTableのslot、refcount、backing storeは別のshared VM-object lockで保護し、取得順を`kernel state → address space（昇順）→ shared VM object → PMM`へ固定した。互換APIは残していない。

同じ4 CPU、direct Sway、5秒animation、guest tick 30–40秒で再測定した。PMM lockは18,363 acquisition中contended 1回（0.005%）、wait 9,250 cyclesで、handled faultは2,953回、平均32,845 cycles、最大2,104,412 cyclesだった。10.858 FPS、frame p99 / max 94 / 95 msで完走し、deadlock、`kernel scheduler unavailable`、Broken pipeはなかった。PMMは分割後もredではないためStep 6のbuddy / magazine gateは未成立のままとする。temporary counterは全削除した。

追加進捗（2026-07-17、effective per-AS fault）: lazy zero-fillと通常COWをprepare / commitへ二相化した。ownerのAS lockは保持してVMAとteardownを直列化する一方、PMM allocationとzero-fillをshared VM-object lock外へ移した。lazy faultはbacking identityのsnapshotと再検証commitだけを共有区間にする。COWはsource pinがまだ無いため、source pageを共有lock内で再検証し、4 KiB copyからcommitまで隙間なく行って解放済みpageを読む競合を防ぐ。commit時にVMO/COW generation、entry、VA、source pageを再検証し、競合winnerがいればcandidate pageを返却する。これにより別ASの通常faultはPMMの長い区間を並行でき、epoch / inflightはAS lock自体を外す将来段階まで追加しない。shared COW table全体のdetachだけは稀なlocked slow pathとして残す。

二相化buildの4 CPU direct Sway＋5秒animationは10.962 FPS、frame p99 / max 95 / 96 ms、input p99 / max 86 / 115 msで完走した。page fault、deadlock、`kernel scheduler unavailable`、Broken pipe、libinput lag warningはなく、QEMUは停止した。その後のsource lifetime reviewでCOW copyを安全な共有区間へ戻し、最終buildを確認した。次はsource pin / object-granular locking、shared COW detachの事前copy、active CPU mask / TLB generation / retire順序へ進む。

## 11. Step 8 — IPC / storage / graphics

Step 0のtimelineで10%以上を占める経路だけを、一つの原因につき一つのcommitで変更する。

IPC / wait:

- 実waiterだけをwakeする。
- 重複wake、空poll、全handle走査を除去する。
- generic wait graphとtimeout非依存の意味は維持する。

storage:

- clean file-backed VMO、DSO、font、shader pageを共有する。
- file VMO cacheとpage cacheの二重保持を避ける。
- repeated paging、metadata I/O、不要なsyncをtimelineから除去する。

graphics:

- DRM submit、fence、page flip、presentation feedbackを対応付ける。
- 不要なfull-frame copyとCPU側直列待ちを除去する。
- threaded llvmpipeのworker数、CPU分散、buffer再利用、damageを測定する。
- live receiverが保持するbufferを再利用しない。

局所改善記録（2026-07-17、input startup logging）: inputdがlibinputの各ioctlを同期serialへ出していた通常時debug logを削除した。同一4 CPU input-only scenarioのcontrolに対し、Sway内のEGL後からEDID処理までが32.925秒から26.148秒へ6.777秒（20.6%）短縮した。animationは11.071から11.104 FPSで同等、inputは6 sampleのため目標値を変更しない。C build、direct Sway、分類、入力、5秒animation、正常終了を確認し、QEMUを停止した。

局所改善記録（2026-07-17、DRM open / close）: card0 openごとの同期uevent publicationを削除した。startup probeで34回あったnetd round-tripはdevice registration時の2件だけになった。normal closeはremote handleをwait / lease fdより先に閉じ、成功closeがorphan reapへ入る競合と、成功時の全handle / KMS table走査・serial出力を削除した。hotplug ABIは追加せず、registration publicationは維持した。

局所改善記録（2026-07-17、file open metadata）: Sway実行後18秒の一回限りのfiled集計でopenat 307回、error 113回、最大114,894,213 cyclesを観測した。成功file openではLPRがdevice判定とsymlink判定のためfstatを最大2回行い、各fstatが4 KiB VMO create / map / IPC / unmap / closeを伴っていた。filedはopen完了時に既にvnode kindを所有するため、未使用だった同サイズのreply fieldへkindを返すようproducer / consumerを同時切替し、通常fileの追加STATを除去した。deviceだけはrdev確認を1回残し、互換分岐は置かなかった。

同変更はuserland全体buildを通した。4 CPU foreground確認では`/bin/sleep 2`がhost実測1.937秒となった。kernelはsignal wakeだけ保存`rax`を`NOT_READY`へ上書きし、LPRはrelative / absolute deadline、handled / ignored signalを分離する。通常timer wakeは`OK`のまま変えない。

間欠stallはnetlink queueにdataが残っていてもgeneric waitが通知tokenをdrainすると、LPRが空の通知FDだけをpollして永久waitするmissed wakeだった。AF_NETLINKもauthoritativeなnetd queue-length pollへ統一した後、EDID、event1 / event0 open、Wayland socket / IPC ready、input readyまで進んだ。

AUTH_MAGIC直前の一回計測は16,176 syscall、92,414,916,332 cyclesだった。上位はclose 17.6%、open 17.3%、readv 16.6%、lseek 13.6%、ftruncate 7.1%で、pollとfutexの合計は約1.5%だった。fstatは96回まで減りopen metadata切替は機能している。次はVFSのFD再lookup、backend state allocation、file data pathを優先し、handled page fault計測は他分類でfaultが残る場合だけ承認を取る。

同分類から、read-only lseekをgeneration検証付きpinned OFD transactionへ変更し、Filed readvはdispatcherの既存pinを再利用してbackend再分類8 lookupを除去した。4 CPU keyboard+tabletの一回確認はsocket 27秒、IPC 39秒で、direct Sway、両device分類、正常終了を通し、Broken pipe、scheduler unavailable、libinput lagはなかった。速度向上値には数えず、目標値は変更しない。

backend stateはopenごとに実型が256 bytes以下でも4 KiB anonymous mmap / zero / munmapしていた。これを15 slotのprocess-local slabへ切り替え、通常lockはatomicだけ、競合時だけfutex wait / wakeする。fork childはtransaction cleanupより前に継承lockをresetする。open / dup / close / fork / pipeの短時間確認と4 CPU keyboard+tablet direct Swayを通し、socket 26秒、IPC 38秒だった。一回値なので速度改善とは判定せず目標値を変えない。

局所改善記録（2026-07-18、Filed close / runtime lock）: warm closeごとに走っていたunused-linked vnodeの多重全件走査をbackend object単位の集計へ縮約し、path walk中の内部closeはrequest境界で一度だけ保守する。file / handle / vnodeの空slot探索はrotating cursor化した。LPRの共通lockとFD table lockは0 / 1 / 2状態とし、競合時だけfutexを使う。single-thread lock elisionは維持し、unlockはthread数でなく取得済みwordを基準にする。

WSL clang buildとFiled VFS / tmpfs / cache invariant testを通した。最初にsingle-thread elisionまで外したbuildがSwayをEGL前で停止させたため取り消し、修正版は4 CPU keyboard+tablet direct Swayをsocket 23秒、IPC 33秒で完走した。Broken pipe、scheduler unavailable、libinput lagはなくQEMUを停止した。一回値なので目標値は変えない。

追加局所改善（2026-07-18、warm path / wait set）: cached intermediate directoryをbackend lookup / statxなしでopenし、cached symlink kindもvnode metadataから判定する。child lookupは検証付きhash hint、transfer leaseは64件のcompact registryへ移し、Filed waitごとの256 handle走査と正常orphan reapの同期serial出力を削除した。

同buildはhost invariant testを通した。4 CPU keyboard+tablet direct Swayは再確認でsocket 18秒、IPC 28秒、分類・正常終了まで通り、対象errorはなかった。直前の一回だけbenchmark shellがIPC ready後にENOMEMとなったが、同一buildで再現せず、lease監査にもcount / lifetime破損はなかったため性能値や原因判定には使わない。QEMUは各回停止した。

追加局所改善（2026-07-18、read / ftruncate）: pin済みread / readvのFD型とFiled backend再探索を除き、read成功時にgenerationを変えず最大32 session × 64 entryを再配布していた走査を削除した。ftruncate後のLPR page cache無効化は約1 MiBのdata zeroから2.5 KiBのmetadata invalidationへ縮小した。

WSL clang build、Filed VFS / tmpfs / cache invariant、4 CPU keyboard+tablet direct Swayを通した。単発はsocket 23秒、IPC 33秒で、直前18 / 28秒より遅くその前23 / 33秒と同じためrun間変動とし、速度改善量・目標値は変更しない。分類・正常終了を確認してQEMUを停止した。

## 12. Step 9 — Phase 4完了判定

最終確認は長時間batteryにせず、次に限定する。

- 4 CPUで独立bootを3回行い、各回でcold Sway、warm Sway、Foot、GTK3 demoを確認する。
- 診断用1 CPUを1回実行し、同じ短時間描画workloadと比較する。
- mouse、tablet、device順序反転を各1回確認する。
- warm Sway IPC readyを9.9秒以内に保つ。
- steady frame intervalの初期目標をp99 50ms以下、最大100ms以下とする。
- input latencyの初期目標をp99 32ms以下、最大100ms以下とする。
- libinputのevent processing lag warningを出さない。
- 4 CPUの描画throughputを1 CPUより初期目標20%以上高くする。
- idle時にbusy loop、周期的全件poll、不要なwakeがない。
- 終了後にprocess、thread、fd、VMOがbaselineへ戻る。
- Sway、Foot、GTK3 demoがBroken pipe、stale processなしで終了する。

目標を変更する場合は測定条件、旧値、新値、理由を記録する。

完了時にtemporary trace、debug counter、dead branch、不要な互換codeを削除し、最終性能表を残す。

## 13. Step 10 — Phase 3B

Phase 4完了後にinput再設計の残りを実装する。

- runtime device add / remove
- remove時のrevoke / HUP
- add / change / remove uevent
- 複数同種device
- controller ownerによるxHCI単一所有
- typed input child export
- USB keyboard / mouse / wheel
- disconnect / reconnect

Phase 3BはPhase 4の性能目標を悪化させない。hotplug 10周、USB実機、長時間resource確認はPhase 5の統合batteryと一緒に行う。
