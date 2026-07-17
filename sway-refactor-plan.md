# Sway / Mesa / LPR 実用化リファクタリング計画

- 作成日: 2026-07-15
- 調査起点: `69fb255` (`main`)
- 状態: 会話で合意した実施計画。実装前の設計基準。

---

## 1. 目的

この計画の目的は、現在の Sway / Mesa / LPR を「専用 fixture と暫定処理で動く実証」から、「upstream バイナリを変更せず、通常の Linux 互換面の上で安定して使える GUI 環境」へ移行することである。

最終状態では、TTY から次のコマンドだけで Sway が起動する。

```sh
sway
```

その上で、次を実現する。

- upstream Sway、wlroots、Mesa、libinput、seatd、Footをバイナリ固有の改変なしで扱う。
- Sway専用launcher、`LD_PRELOAD` shim、プロセス名による特別分岐を廃止する。
- SwayのキーマッピングからFoot、bemenu、Thunarを起動する。
- keyboard、relative mouse、absolute tabletを安定して扱う。
- koboxのUSB mouse / keyboardを、他のUSB device classと衝突しないuserland構成で扱う。
- APを実際に起動し、4 CPUでrace、hang、resource破壊なく動作する。
- warm状態のSwayを一桁後半、具体的には9.9秒以内で操作可能にする。
- GUIアプリケーションを実用上十分な速度で操作できる。
- SwayとFootを少なくとも10回起動・終了しても、resource leak、累積遅延、終了失敗を起こさない。
- fd、process、device、wait、service ownershipを観測しやすくし、追加・廃止・修正を容易にする。
- `tests/` 配下に正式なbatteryを置き、上記の契約を継続して守る。

本計画は現在の実装状態をgolden baselineとして固定しない。現在動いている経路から事実と再現方法は回収するが、暫定実装、誤った成功判定、リーク許容、専用hackを維持する理由にはしない。

---

## 2. 関連文書との関係

- `pacha_docs/refactor-plan.md` は、これまでのLPR、Mesa、DRM、input、Sway到達履歴と実測記録として参照する。
- `pacha_docs/fd-ops-design.md` は、fd周辺の問題一覧と過去に検討した契約の資料として参照する。
- `pacha_docs/smp-handoff-m36e.md` は、旧SMP試行で成功・失敗した事実の引き継ぎ資料として参照する。
- 本文書は、今後のSway実用化、fd汎用化、SMP、input再設計、性能改善の実施順と受け入れ条件を定める。
- 上記文書と本計画が衝突する場合、Sway実用化の今後の方針については本計画を優先する。
- 過去の24 step fd全面移行は、そのまま再開しない。必要な契約は再利用するが、実装は需要ドリブンの縦方向sliceで進める。

---

## 3. 現在地

### 3.1 到達済みの機能

現在までに、少なくとも次の機能は実際の経路で到達している。

- upstream Sway 1.10.1
- wlroots 0.18.2
- Mesa 25.1.9 / llvmpipe
- libinput 1.28.1
- seatd / libseat 0.9.1
- virtio-gpuによるDRM/KMS
- Mesaからの描画とpage flip
- Wayland compositorのfirst frame
- wl_shm clientの表示
- keyboard、relative mouse、button入力
- 手動での基本的なSway操作
- 制限付きのSway再起動試験
- upstream `wayland-info` の起動
- upstream Footのwindow表示までの到達

これらは「必要な主要部品が動き得る」証拠であり、現在の構造を維持する根拠ではない。

### 3.2 現在の主要な問題

#### Sway専用launcher

`userland/fixtures/src/wsl_musl/lpr_sway_launcher.c` が、現在は次の責務をまとめて持っている。

- seatdの起動
- seatd socketの削除
- Sway固有環境変数の設定
- `LP_NUM_THREADS=0` の設定
- keymap用`LD_PRELOAD`の設定
- Wayland socket、IPC socket、first frameの待機
- clientの起動
- Swayのsignal送信、kill、wait、reap
- fixture用markerとresource checkpointの出力

これは製品のsession管理、Linux互換、同期、テスト制御を一つのSway専用バイナリへ集めた状態である。ファイルだけを先に削除せず、各責務を正しい汎用層へ移した後に削除する。

#### fdとservice objectのライフサイクル

- LPRは単一fd tableへ移行済みだが、close、dup、fork、exec、poll、epoll、SCM_RIGHTSにkind別分岐が残る。
- Linux-visible fdとnative fdの境界が明確でない箇所がある。
- SCM_RIGHTSはFILED、INPUT、DRMなどの種類をwire上で特別扱いしている。
- service objectのowner、export、receiver、queue中参照の寿命が統一されていない。
- 強制終了時のfiled / drmd resource leakを現在のenduranceが許容している。
- drmdのopen時orphan回収は、終了時回収の代わりにはならない。

#### process notificationと終了

- SIGTERMで正常終了せず、SIGKILLへescalateする場合がある。
- 子process監視を周期pollへ寄せる暫定経路がある。
- pending signal handlerをLPRのCコードから直接呼ぶ方式は、通常のsignal frameと整合しない。
- Footに必要なPTY、SIGCHLD、wait、shell lifecycleは表示成功だけでは完成していない。

#### Mesa / wlroots互換

- wlroots keymap生成はSway限定preloadに依存している。
- `LP_NUM_THREADS=0` は、fake PRIME経路にproducer completion / fenceが不足していることを隠している。
- shader cacheを無効化するfixtureが残る。
- Footが利用するrandom、passwd、fallocate / punch-holeなどのLinux互換面に不足がある。

#### SMP

- AP起動用の実装はあるが、通常bootでは有効化されていない。
- 旧`smp-wip`では4 CPU起動とAP上のsyscall / IPC実行まで到達した。
- allocator、remote wake / stop、timer preemption、TLB shootdown、teardownが未完成で、回帰もgreenではない。
- 旧ブランチを現在の`main`へ一括mergeできる状態ではない。

#### input

- `INPUTD_DEVICE_COUNT=2` が固定されている。
- seed0bootとinputdがvirtio-inputを2台だけ扱う。
- event0をkeyboard、event1をmouseとして扱う経路がある。
- sysfs、netlink uevent、PCI BDF、device roleが静的に結び付いている。
- LPRの`/dev/input/eventN`処理に一桁前提がある。
- production経路はvirtio-input中心で、USB xHCI / HIDの所有者が決まっていない。

---

## 4. 基本原則

### 4.1 正しさを最優先する

各phaseでは、速度より先に次を成立させる。

- ownershipが一意で説明できる。
- retainとreleaseが対応する。
- close、fork、exec、killの結果が順序に依存しない。
- process終了後にそのprocessだけが所有していたresourceが残らない。
- transfer済みのresourceを元ownerの終了で壊さない。
- signal、wait、pollがbusy loopや偶然のtimeoutに依存しない。
- SMPでも同じ意味論を維持する。
- data corruption、use-after-free、double close、lost wakeupを残さない。

現在の挙動がLinuxの意味論や上記の契約と違う場合、現在の挙動を保存しない。

### 4.2 固定するのは環境と契約であり、現在の実装ではない

比較可能性のため、次は固定する。

- QEMU / KVMの実行方法
- vCPU数
- memory量
- disk imageの生成条件
- cold / warmの定義
- input device profile
- 計測区間
- resource snapshotの項目
- upstream binaryのversion

一方、次は基準として固定しない。

- launcherのmarker
- sleep後の擬似first-frame
- forced exit時のリーク許容値
- 固定fd番号
- event0 / event1の役割
- 固定cache slot数
- 次回open時のorphan cleanup
- `LP_NUM_THREADS=0`
- shader cache無効化

### 4.3 userlandでできることをkernelへ入れない

kernelは次の機構だけを担当する。

- CPU起動
- schedulingとpreemptionに必要なcontext
- address spaceとTLB同期
- memory allocatorの同期
- thread / processの実行状態
- IPIとCPU間同期
- 既存native object / fd / IPC機構

次はuserlandが担当する。

- seatd起動
- XDG runtime directory
- Sway設定
- device discoveryとrole判定
- sysfs / uevent表現
- USB class policy
- DRM / input service ownership
- Linux fd / OFD意味論
- signal dispositionとsession lifecycle

kernelを変更する前に、userlandで解決できないこと、kernelが担当すべき理由、対象file、守るinvariant、red testを提示し、明示的な許可を得る。

### 4.4 upstreamバイナリを特別扱いしない

禁止するもの:

- Sway、Mesa、wlroots、Footのbinary patch
- process名を見たLPR内部の分岐
- Swayだけを対象にしたsyscall semantics
- Sway専用`LD_PRELOAD`
- launcherでのfd書き換え
- fixed fdを前提とした継承
- テストを通すためだけの成功status正規化

許容するもの:

- upstreamが正式に提供するbuild option
- 通常の`/etc/sway/config`
- session全体の標準環境
- seatdの通常service起動
- PachaOSにLinux VTがないことを表す`SEATD_VTBOUND=0`
- llvmpipe利用を明示する`WLR_RENDERER_ALLOW_SOFTWARE=1`

`WLR_RENDERER_ALLOW_SOFTWARE=1` はSway専用launcherへ隠さない。hardware rendererがない間のplatform設定として明示し、将来hardware rendererを導入した時点で削除可能にする。

### 4.5 全面書き換えではなく、完成した縦方向sliceを積む

一つのsliceは原則として次の順で行う。

1. Linuxまたは本計画上の正しい契約をtestにする。
2. 必要な観測を追加する。
3. 一つの実workloadを入口から終了まで通す。
4. 汎用実装へ移す。
5. 古い特別経路を削除する。
6. correctness batteryを通す。
7. 性能値とresource値を記録する。

一つのkindを半分だけ新構造へ移して長期間dual pathを残さない。移行単位は小さくするが、sliceの終端ではowner、close、wait、transferを一貫させる。

### 4.6 ABI変更

ABI変更が必要な場合は、実装前に次を記録する。

- 現ABIでは表現できない契約
- ABI変更が必要な具体的理由
- producerとconsumerの一覧
- 番号とlayoutの変更
- 移行を一括で行えること
- regression test

互換性維持は行わない。新しいoperationを「連番をずらしたくない」という理由で末尾へ追加しない。意味上正しい位置へ配置し、後続番号をすべて更新し、全producer / consumerを同じ変更で追従させる。compat shimや旧version分岐を残さない。

kernel ABI変更は現時点では予定しない。必要性が生じた場合は、kernel編集許可とは別に理由を説明する。

---

## 5. 移行中の性能方針

### 5.1 許容する性能劣化

汎用化と正しさの確立中は、次の一時的な性能劣化を許容する。

- backend abstraction追加による一定のcall overhead
- ownership traceとresource counterによる負荷
- 正しいlock導入直後の並列性能低下
- SMP初期段階のglobal lock
- bounded cache導入直後のcold miss増加
- 正しいwait graphへ移行する途中のwake回数増加
- debug buildでの起動時間、frame rate、memory使用量の悪化

これらはcorrectness gateを失敗させない。ただし、理由、計測条件、差分、後で除去または最適化する対象を記録する。

### 5.2 移行中でも許容しない崩れ

次は性能課題として先送りせず、そのphaseで調査・修正する。

- 起動や終了のたびに時間またはresourceが増え続ける。
- idle中にCPUを消費し続ける。
- event通知があるのに固定timeoutまで待つ。
- busy loop、短周期poll、無制限retryへ戻る。
- fd数、device数、surface数に対して意図せず二次関数的に悪化する。
- frameまたはinputが周期的に数百ms停止する。
- 1 CPUでは正常だが4 CPUで極端に遅くなる。
- cache有効化により破損、hang、極端な悪化が起きる。
- 正常終了が性能変更によってtimeoutまたはSIGKILL依存になる。
- 説明できない大幅な性能変化が再現する。
- 汎用化したはずの経路が特定kindのfallbackへ落ちる。

### 5.3 性能変化の判定手順

各slice終了時に次を行う。

1. 同一artifact、同一disk条件で複数回測定する。
2. coldとwarmを分離する。
3. CPU使用率、wake回数、wait時間、I/O、fault、frame timeを確認する。
4. 変化を「意図した移行コスト」「計測ノイズ」「構造的異常」「未説明」に分類する。
5. 構造的異常はその場で修正する。
6. 意図した移行コストは性能負債としてPhase 4へ送る。
7. 未説明の大幅な変化は、原因を限定できるまで次の大きなphaseへ進めない。

一度greenになったcorrectness oracleを、性能上の都合で緩めない。

---

## 6. Phase 0 — 現状の解体・判定・回復

このphaseは現在の状態をbaselineとして固定する工程ではない。現在の実装から有効な事実を回収し、正しい契約と観測手段を用意する工程である。

### P0.1 現経路の分類

Sway / Foot関連の変更を、次の単位で分類する。

- packageとrootfs
- PTY
- SIGCHLD / wait
- filed / tmpfs / memfd
- LPR fd table
- SCM_RIGHTS
- drmd object ownership
- DRM buffer reuse
- seatd/session
- wlroots keymap
- inputd
- launcher / fixture
- performance instrumentation

各変更を次のいずれかに分類する。

- そのまま残す。
- 契約は正しいが実装を作り直す。
- 診断用途に限って一時的に残す。
- 廃止する。
- 証拠不足のため保留する。

現時点で少なくとも次の扱いを予定する。

- upstream Footと`wayland-info`の起動到達は、互換面の証拠として残す。
- PTYの動的indexという要求は残すが、汎用fieldへ無理に型を格納する表現は再設計する。
- SIGCHLDの周期pollとhandler直接呼び出しは廃止する。
- drmdの次回open時orphan回収は診断用途に限定し、leaseへ置換する。
- live receiverを壊し得るexport ref強制解除は廃止する。
- 根拠のないcache slot縮小・拡大は採用しない。
- launcherのmarkerは一時観測に限り、製品成功判定に使わない。

### P0.2 正しいoracleの先行作成

次の期待値を最初から正しい値でtestにする。

- normal exitでstatus 0
- SIGTERMでescalation 0
- direct SIGKILL後もservice resourceが回収される
- child process差分0
- LPR fd / native fd差分0
- filed handle / session差分0
- drmd handle / FB / dumb buffer差分0
- netd socket / queued transfer差分0
- seatd終了後のsocket残存0
- resource回収が次回openに依存しない

未実装のためredであっても期待値を緩めない。expected-redとして原因と担当phaseを記録し、greenへ反転した後は再び許容値へ戻さない。

### P0.3 実際のreadiness観測

launcherのsleepやIPC socket出現ではなく、次のeventを独立して観測できるようにする。

- TTYからのexec要求
- Sway process開始
- seat取得
- DRM open
- renderer初期化
- Wayland socket publish
- output最初のcommit
- 最初のpage flip completion
- 最初のclient surface map
- 最初のkeyboard / pointer event処理

first frameは実際のoutput commitとscanout結果で判定する。IPC socketが見えたことや一定時間生存したことをfirst frameとしない。

### P0.4 resource snapshot

最低限、次を同一形式で取得する。

- process / thread数
- LPR fd entry / OFD / backend object数
- native fd数
- pending transfer数
- filed session / open handle / cached VMO
- drmd session / GEM / dumb buffer / FB / exported object
- netd local socket / SCM queue
- inputd device / client / open handle
- kernel process / thread / VMO / mapped page / free page

「現在値を正解として固定」するためではなく、同じboot内の操作前後でresource conservationを確認するために使う。

### P0.5 正式batteryの骨格

`tests/` 配下へ、最終的に次を束ねる正式runnerを置く。

- host unit / static test
- userland service unit test
- 1 CPU QEMU correctness
- 4 CPU QEMU correctness
- Sway / Foot integration
- input profile
- endurance
- filesystem fsck
- performance

候補名は`tests/run-sway-refactor-battery.sh`とする。成果物は`.artifacts/sway-refactor-battery/`以下へ分離する。

失敗したcorrectness testを自動retryで成功扱いしない。infra障害を再試行する場合も、最初の失敗を記録し、flakeとして扱う。

### Phase 0 完了条件

- 現経路のkeep / rework / diagnostic / delete / hold分類が完了している。
- launcherのsleepを使わずに実frameとclient mapを区別できる。
- resource snapshotを同一bootの前後で比較できる。
- 既知のリークと終了失敗が、許容付きgreenではなく明示的なredになっている。
- 既存の無関係なgreen testを壊していない。
- 現在の性能値は参考値として記録するが、受け入れbaselineにはしていない。

---

## 7. Phase 1 — fd、ownership、wait、Linux互換の汎用基盤

このphaseでは、SwayとFootの実lifecycleを最初の完成した縦方向sliceにする。fd抽象化全体を先に完成させるのではなく、実際に必要なbackendを一つずつ新契約へ移す。

### P1.1 fd entry / OFD / backendの契約

責務を次の3層へ分ける。

#### fd entry

- Linux-visible fd number
- `FD_CLOEXEC`
- OFD参照
- process固有のentry lifetime

#### open file description

- status flags
- offsetまたはcounter
- seals
- backend参照
- shared lifetime
- wait source

#### backend object

- service handleまたはnative object
- read / write / ioctl / mmap
- poll source
- export / import
- close
- owner lease

kind固有payloadへ共通状態を重複させない。fd number、native fd、service handleを同じ整数空間として暗黙に扱わない。

最初の移行対象は、Sway / Footで必要な次の順を基本とする。

1. process / child notification
2. TTY / PTY
3. local socket
4. FILED regular file / memfd
5. DRM / dma-buf
6. INPUT
7. epoll

実測依存関係により順序を変える場合は、理由を記録する。

### P1.2 service lease

service objectの参照を少なくとも次に区別する。

- creator / owner
- local fd entry
- duplicateされたOFD
- exported transfer
- queue中transfer
- imported receiver
- mapping ticket
- in-flight request

要求する性質:

- sender Aからreceiver Bへ渡した後、AをkillしてもBのresourceは有効である。
- BからCへ再転送できる。
- Bをkillした場合、Bだけのleaseが即時回収される。
- queue中にsenderまたはreceiverが死んでも二重解放しない。
- close順序に依存しない。
- cleanupは次のopen、再接続、cache evictionを待たない。
- service crash時は、残存clientが明確なHUP / errorを受ける。

filed、drmd、netd、inputdを同じownership原則へ揃える。

### P1.3 汎用SCM_RIGHTS

SCM_RIGHTSをkind別metadata copyから、opaque backend transferへ変更する。

必須test:

- A→B
- A→B→C
- dup後のtransfer
- CLOEXEC
- sender close前後
- sender kill
- receiver kill
- queue中kill
- low fd / high fd
- multiple fd batch
- read-only rights
- mmapを保持したままfd close
- FILED memfd
- INPUT
- DRM / dma-buf

wireからSway専用またはkind固有の特別なtransfer表現を除去する。すべてのbackendを一度に対応させず、対応済みbackendだけが明示的なexport / import operationを持つ。

### P1.4 汎用wait graph

epollと各backendの待機を、wait sourceのgraphとして整理する。

- pipe
- socket
- eventfd
- input
- DRM completion
- child process state
- service HUP
- nested epoll

固定10ms pollや「non-native fdだから定期確認する」という経路を最終的に除去する。child notificationはprocess固有のwait sourceとしてsupervisorからLPRへ配送する。

### P1.5 SIGCHLD / wait / signal frame

- child exit / stop / continueをprocess固有通知にする。
- `waitpid` / `waitid`が通知を失わないようにする。
- SIGCHLD dispositionとmaskを通常のsignal処理へ統合する。
- LPRのCコードからuser handlerを直接呼ばない。
- normal signal frame、altstack、restorer、return経路を使う。
- Footが起動したshellを正しくreapできる。
- 子processがいない場合に周期pollしない。

### P1.6 Linux互換面

upstream GUI stackに必要な次を、汎用Linux互換として完成させる。

- `/dev/shm`
- `memfd_create`
- `F_ADD_SEALS` / `F_GET_SEALS`
- sparse tmpfs
- `fallocate`
- `FALLOC_FL_PUNCH_HOLE`
- shared / private mmapの正しいlifetime
- `getrandom` または`/dev/urandom`
- passwd / group / `getpwuid`
- 動的devpts
- `TIOCGPTN`
- PTY indexの複数同時利用と回収
- local socket / socketpair
- SCM_RIGHTS
- `SIGCHLD` / wait
- `/run/user/<uid>`
- `XDG_RUNTIME_DIR`

fixed capacityの増加だけで問題を隠さない。capacityへ到達した場合は、所有権、eviction、fragmentation、未解放参照を測る。

### P1.7 wlroots keymap

- Sway限定の`shm_open` preloadを削除する。
- upstream wlrootsが通常のfeature detectionで`memfd_create`経路を選ぶようにする。
- wlroots sourceへPachaOS固有patchを入れない。
- keymap fdのSCM_RIGHTS、mmap、close lifetimeを汎用testでも検証する。

### P1.8 Mesa producer / consumer同期

`LP_NUM_THREADS=0`で競合を避けるのではなく、fake PRIME / dma-buf経路に正しいcompletion契約を入れる。

- producer submit
- rendering completion
- buffer export
- consumer acquire
- scanout
- release / reuse

どの時点で書き込み完了と見なすかを明示し、必要なfenceまたはcompletion tokenをDRM / VMO transactionへ持たせる。`glFinish`を常時挿入するだけの解決にしない。

正しい同期が成立した後にthreaded llvmpipeを有効化し、`LP_NUM_THREADS=0`を削除する。

### P1.9 seatdとsession環境

seatdをSway専用launcherの子として起動しない。通常のuserland serviceまたはsession bootstrapが次を用意する。

- seatd daemon
- seatd socket
- `/run/user/<uid>`
- `XDG_RUNTIME_DIR`
- `LIBSEAT_BACKEND=seatd`
- `SEATD_VTBOUND=0`
- software renderer利用中の`WLR_RENDERER_ALLOW_SOFTWARE=1`

Sway終了時にseatdまで毎回破棄する構成を標準にしない。service lifecycleとuser session lifecycleを分ける。

### P1.10 direct Swayとlauncher削除

次を満たしてから`lpr_sway_launcher.c`を削除する。

- TTYから`/usr/bin/sway`を直接execできる。
- `command -v sway`がwrapperではなく`/usr/bin/sway`を指す。
- Sway process treeに専用launcherがいない。
- seatdが通常serviceとして利用できる。
- keymap preloadがない。
- `LP_NUM_THREADS=0`がない。
- shader cache無効化がない。
- socket unlinkをlauncherが行わない。
- first-frame待機をlauncherが行わない。
- normal exit、SIGTERM、SIGKILLのresource回収testがlauncherなしで動く。

削除対象には次も含める。

- launcherのbuild target
- pack entry
- launcher専用fixture引数
- launcher専用環境変数
- launcher markerだけを読むtest code
- launcherだけのcleanup処理

### Phase 1 完了条件

- upstream SwayをTTYから直接起動できる。
- upstream FootがSwayから起動し、PTY上のshellが利用できる。
- normal exitとSIGTERMでescalationしない。
- direct SIGKILL後にも全service resourceが操作前へ戻る。
- Sway / Footを10回繰り返してresource差分0。
- SCM_RIGHTS A→B→Cが対応backendで成立する。
- child通知の周期pollがない。
- keymap preload、`LP_NUM_THREADS=0`、shader cache無効化がない。
- `lpr_sway_launcher.c`と全参照が削除されている。
- 性能低下が残る場合、正しさを崩さず測定・説明されている。

---

## 8. Phase 2 — SMP / AP有効化

このphaseはkernel変更を含む。開始前に、対象file、red test、invariant、userlandでは解決できない理由を提示し、明示的な許可を得る。

- signal stateの256 thread固定 動的に
- native COW table容量 半永久化 も一緒に行う

### Phase 2 完了条件

- 4 CPUが実際にonlineである。
- user threadがAP上でsyscall、IPC、page fault、file I/Oを実行する。
- allocator / COW / fork / exec / kill / teardown stressがgreen。
- remote wake / stop / killがtimeoutに依存しない。
- TLB shootdown testがgreen。
- 4 CPU bootを20回連続で完了する。
- Sway / Foot lifecycleを4 CPUで10回通し、resource差分0。
- 1 CPU専用fallbackを通常成功経路として残さない。
- kernelへSway、seatd、device pathなどのuserland知識を追加していない。

---

## 9. Phase 3 — input / libinput再設計

### P3.1 動的device registry

`INPUTD_DEVICE_COUNT=2`を廃止し、device registryを導入する。

deviceは少なくとも次の属性を持つ。

- stable internal id
- bus / controller identity
- vendor / product
- capability bitmap
- relative axis
- absolute axisとrange
- keyboard keys
- buttons
- current state
- open client
- generation
- add / remove / revoke状態

event番号をdevice roleとして使わない。

### P3.2 discoveryとpublication

- seed0bootが固定2台を要求しない。
- inputdが可変個数を受け取る。
- runtime add / removeを扱う。
- `/dev/input/eventN`を動的に割り当てる。
- 一桁を越えるevent番号を扱う。
- sysfsをruntime device情報から生成する。
- netlink ueventをadd / change / removeで発行する。
- PCI BDFとevent番号をhardcodeしない。

static sysfs generatorは、製品情報源ではなくtest fixture生成へ限定するか削除する。

### P3.3 evdev semantics

- `EVIOCGNAME`
- `EVIOCGID`
- `EVIOCGBIT`
- `EVIOCGABS`
- `EVIOCREVOKE`
- nonblocking read
- poll / epoll
- device removal時のHUP
- event ordering
- SYN_REPORT
- key state
- absolute range

libinputがfallbackや警告を出した場合、警告を消すだけの偽ioctl responseを返さず、要求される意味を実装する。

### P3.4 virtio mouse / tablet

QEMU input profileを明示的に選択可能にする。

- virtio keyboard + relative mouse
- virtio keyboard + absolute tablet
- keyboard only
- mouse only
- 複数同種device
- device順序入れ替え
- hotplug / remove

QMP input injectionで次を検証する。

- key press / release
- relative X / Y
- left / right button
- absolute X / Y
- edge / center座標
- add後の利用
- remove後のrevoke

### P3.5 libinput / wlroots統合

raw evdev test、libinput test、Sway end-to-end testを別々に残す。

- raw evdevが正しい。
- libinputが正しいdevice typeを選ぶ。
- wlrootsがudev経由で列挙する。
- Swayがkeyboard / mouse / tabletを扱う。
- device順序が変わっても同じ操作ができる。
- device removalでSwayがhangしない。

raw evdevを「libinputで間接的に通る」としてbatteryから除外しない。

### P3.6 USB controller ownership

xHCI controllerをinputdとstorage serviceが別々に所有しない。

一つのuserland controller-ownerまたはhardware islandが次を担当する。

- xHCI controller ownership
- enumeration
- endpoint lifetime
- disconnect
- class driverへのtyped child export

HID keyboard / mouseはtyped input childとしてinputdへ渡す。mass storageなどの別classは対応serviceへ渡す。同じphysical controllerを複数serviceが直接操作しない。

kernelへUSB device class policyを入れない。

### P3.7 USB HID

- keyboard report
- relative mouse report
- modifier
- button
- wheel
- disconnect / reconnect
- 複数device

まずsynthetic / QEMU相当の再現testを作り、その後koboxの実USB mouse / keyboardで確認する。hardware確認は自動batteryとは別に記録するが、最終完了条件には含める。

### Phase 3 完了条件

- event0 / event1の役割固定がない。
- 可変個数とmulti-digit event nodeを扱う。
- virtio-mouseとvirtio-tabletの両方でSwayを操作できる。
- device順序変更とhotplugで動作する。
- USB controllerの所有者が一つである。
- koboxのUSB keyboard / mouseで基本操作できる。
- device add / removeを10回繰り返してresource差分0。
- raw evdev、libinput、wlroots/Swayの3層testがgreen。

---

## 10. Phase 4 — 起動・描画・入力性能

Phase 1から3で記録した性能負債を、正しい契約を崩さずに解消する。

### P4.1 起動timeline

次の区間をguest monotonic clockで記録する。

- TTY Enter → Sway exec
- exec → seat open
- seat open → DRM open
- DRM open → renderer ready
- renderer ready → Wayland socket
- Wayland socket → first output commit
- output commit → page flip completion
- Wayland socket → Foot map
- input inject → client event

host wall timeも補助として残すが、host schedulingの揺れとguest処理時間を混同しない。

### P4.2 起動時の無駄の除去

優先調査対象:

- DRM uevent storm
- 不要なdevice再列挙
- socket / childの周期poll
- timeoutまで待つ経路
- shader cache
- fontconfig cache
- passwd / locale fallback
- repeated DSO paging
- file VMO cache miss
- rootfs metadata I/O
- seatd再起動
- 不要なsync / fsync

sleep時間を短くするだけの高速化にしない。通知または実eventで次へ進む。

### P4.3 Mesa / DRM

- threaded llvmpipe
- worker数とonline CPU数
- fence / completion待機
- displaytargetからscanoutへのcopy
- damage rectangle
- transfer_2d / flush
- page flip batching
- VMO map / unmap
- GEM / dumb buffer再利用
- shader cache

buffer poolはbyte budget、entry budget、in-use state、eviction policyを持つ。live receiverのbufferを再利用しない。固定slot数を上下させるだけの調整にしない。

### P4.4 filed / tmpfs / native fd

- file VMO cacheのhit / miss / eviction
- cache byte budget
- sparse tmpfs page lookup
- memfd mapping lifetime
- native fd上限
- fragmentation
- page cacheとの二重保持
- fallocate / punch-hole

cache削減で一時的に成功しても、10回以上の再起動で枯渇または性能低下する場合は採用しない。

### P4.5 input latency

- IRQ受信
- inputd read
- evdev publish
- libinput dispatch
- wlroots event
- client event

各時刻を対応付け、timer late、event coalescing、poll quantum、scheduler wakeを分離する。SMPによって遅延が増える場合はCPU affinityで隠さず、wake / lock / schedulingの原因を修正する。

### P4.6 一時計測の除去

最適化完了後、次を整理する。

- 大量serial print
- hot pathのtemporary counter
- process名限定trace
- P6_PAGE_DIAG等の一時marker
- testのためだけのsleep
- 実装判断に使わなくなった環境変数

必要な可観測性はruntime mask付きの構造化traceとresource snapshotとして残す。

### Phase 4 完了条件

- 基準QEMU / KVM構成でwarm Swayが9.9秒以内に操作可能。
- 計測はlauncher sleepではなく実eventに基づく。
- threaded llvmpipeが有効。
- idle時に周期poll負荷がない。
- Sway / Footの再起動ごとに速度が悪化しない。
- keyboard / pointerに周期的な数百ms停止がない。
- 4 CPUが1 CPUより代表的GUI workloadで有効に働く。
- cold値とwarm値を別々に記録している。
- 移行中に許容した性能負債が、解消済みまたは明確な理由付きで残件化されている。

Foot、bemenu、Thunarの個別起動時間とinput latencyの数値目標は、正しいtimelineが完成した最初の測定で決める。現在のlauncher由来の値から目標を作らない。決定後は本文書へ追記し、理由なく緩和しない。

---

## 11. Phase 5 — 実用操作とアプリケーション

### P5.1 標準Sway設定

`/etc/sway/config`へ少なくとも次を設定する。

- Mod+Enter: Foot
- Mod+d: bemenu
- Mod+e: Thunar
- window focus
- window move
- window close
- workspace切替
- keyboard layout / repeat
- pointer
- tablet absolute pointer

unsupportedなXwaylandを暗黙に起動して待たない。未対応なら設定で明示的にdisableし、Wayland-nativeアプリを検証対象とする。

### P5.2 Foot

- window map
- shell起動
- keyboard入力
- command実行
- stdout / stderr表示
- resize
- close
- child reap
- 再起動

random、passwd、PTY、SIGCHLD、fallocateの不足をFoot専用fallbackで隠さない。

### P5.3 bemenu

- Sway keybindから起動
- keyboard入力
- 候補選択
- 選択したcommand起動
- cancel
- 再起動

### P5.4 Thunar

Thunarは単なるwindow表示ではなく、GTK、font、icon、directory listing、file access、child process起動を含む互換確認として使う。

- window map
- directory表示
- keyboard / pointer操作
- file選択
- directory移動
- application launchまたはopen action
- close
- 再起動

DBusや補助serviceが必要になった場合は、Thunar専用stubを入れず、通常のuserland serviceとして必要性と範囲を判断する。

### P5.5 基本操作確認

自動testと手動testの両方を使う。

自動:

- QMP key injection
- QMP relative / absolute pointer
- screendump
- Wayland client state
- process tree
- resource snapshot

手動:

- focus
- move
- resize
- workspace
- Foot操作
- bemenu操作
- Thunar操作
- virtio-tablet
- kobox USB keyboard / mouse

### Phase 5 完了条件

- TTYで`sway`と入力するだけで起動する。
- SwayのkeybindからFoot、bemenu、Thunarが起動する。
- keyboard、virtio-mouse、virtio-tabletで基本操作できる。
- kobox USB keyboard / mouseで基本操作できる。
- Swayを10回起動・終了できる。
- Footを10回起動・終了できる。
- bemenuとThunarを繰り返し起動しても累積劣化しない。
- normal exitでSIGKILL escalationがない。
- direct kill後もresource差分0。
- warm Sway起動9.9秒以内。

---

## 12. 正式battery

testは最後にまとめて追加するのではなく、各phaseの最初にredを追加し、実装とともにgreenへ反転する。最終的に`tests/run-sway-refactor-battery.sh`から実行できる形へ統合する。

### 12.1 host / unit

- fd entry / OFD refcount
- dup / dup2 / CLOEXEC
- fork / exec snapshot
- SCM_RIGHTS A→B→C
- service lease
- transfer queue kill
- PTY index allocator
- tmpfs sparse page
- seals / fallocate / punch-hole
- device registry
- sysfs / uevent生成
- SMP allocator invariant
- thread state transition
- CPU mask / TLB shootdown model
- service ABI layout

### 12.2 1 CPU QEMU

- fd-pipe
- pipe stress
- local socket / socketpair
- epoll / nested epoll
- async signal
- SIGCHLD / wait
- PTY multiple index
- shared mapping
- memfd / seals
- state leak
- ext4 persistence
- shader cache + post-run fsck
- raw evdev
- libinput
- DRM / page flip
- Mesa
- direct Sway
- Foot
- bemenu
- Thunar

### 12.3 4 CPU QEMU

- boot 20回
- allocator stress
- COW
- fork / exec
- process kill
- remote wake
- timer preemption
- TLB shootdown
- service IPC
- filed I/O
- Mesa threaded rendering
- direct Sway
- Sway / Foot endurance

### 12.4 input profile

- keyboard + relative mouse
- keyboard + absolute tablet
- device順序入れ替え
- 複数mouse
- hotplug
- remove / revoke
- USB HID synthetic
- kobox実USB hardware

### 12.5 endurance

最低限、次の各iterationで操作前後のresource snapshot一致を要求する。

- Sway normal exit ×10
- Sway SIGTERM ×10
- Sway direct SIGKILL ×10
- Foot normal exit ×10
- Foot child shell exit ×10
- input add / remove ×10

必要に応じて最終batteryでは20回以上へ増やす。回数を増やして上限到達を遅らせるのではなく、各iterationで差分0を確認する。

### 12.6 performance

correctness batteryとperformance測定を分ける。移行中の速度低下でcorrectnessをredにしない一方、測定不能、hang、累積悪化、busy loopはcorrectness側でもfailにする。

- Sway cold / warm
- Foot cold / warm
- bemenu cold / warm
- Thunar cold / warm
- frame time
- input latency
- idle CPU
- 1 CPU / 4 CPU比較
- 1回目 / 10回目比較

### 12.7 battery運用規則

- 同一test内では同一artifactを使う。
- runtime artifactをrepo rootへ置かない。
- artifactは`.artifacts/`以下へ置く。
- correctness failureをretry成功で隠さない。
- timeoutを延ばすだけでgreenにしない。
- screendumpだけでprocess lifecycleを省略しない。
- serial markerだけで実frameを省略しない。
- 手動確認だけで自動regressionを省略しない。
- hardware依存testは環境不足をskipとして明示し、実施記録を残す。

---

## 13. デバッグ容易性

最終構造では、少なくとも次を一回の操作で取得できるようにする。

- process tree
- thread stateと実行CPU
- LPR fd table
- OFD共有関係
- backend objectとservice lease
- pending SCM transfer
- wait graph
- epoll interest
- filed object / cache
- drmd object / FB / buffer / export
- input device registry
- active address space CPU mask
- pending TLB shootdown

traceはruntime maskで有効化でき、通常時はhot pathを大きく劣化させない。個別fixtureへprintfを追加し続ける方式にしない。

error時には、単なるtimeoutではなく、最後に待っていたobject、owner、generation、対象CPUを残す。

---

## 14. 禁止する解決方法

- kernelへSway、seatd、storage_boot、module path、device roleの知識を入れる。
- capacity定数を増やすだけでリークを解決したことにする。
- cacheを無効化して破損を隠す。
- threadを無効化して同期不備を隠す。
- CPUを1個へ戻してSMP testを成功扱いする。
- fixed fd、fixed PID、fixed BDF、fixed event番号へ依存する。
- sleep、短周期poll、timeout延長でraceを隠す。
- SIGTERM失敗をSIGKILL成功として正規化する。
- live receiverを壊す一括revoke。
- process名に応じたLPR分岐。
- upstream binaryへのpatch。
- test fixtureだけで成立する別実装。
- ABIの末尾へ無関係なoperationを追加する。
- old / new ABIのcompat pathを残す。

---

## 15. 実施順

原則として次の順で進める。

1. Phase 0: 現状の解体・判定・回復
2. Phase 1前半: fd / OFD / lease / transfer / wait
3. Phase 1後半: Linux互換、wlroots、Mesa同期、session、direct Sway
4. `lpr_sway_launcher.c`削除
5. Phase 2: SMP / AP有効化
6. Phase 3: dynamic input、virtio-tablet、USB HID
7. Phase 4: 起動・描画・入力性能
8. Phase 5: Foot、bemenu、Thunar、基本操作
9. 全battery、hardware確認、最終cleanup

Phase 1のoptional backendすべてが完成するまでSMPを待つ必要はない。ただし、Sway / Footが使うownership、wait、kill、resource cleanupの縦方向sliceは、APへ通常user threadを載せる前にgreenにする。

各実装単位では次を守る。

1. redまたは契約test
2. 観測
3. 最小の汎用修正
4. correctness確認
5. 古い経路削除
6. 性能記録
7. 一時診断削除

挙動を変えない機械的移動と、意味論を変える修正を同じcommitへ混ぜない。

---

## 16. 最終受け入れ条件

### 起動とupstream性

- TTYで`sway`と入力するだけで起動する。
- `sway`は専用wrapperではなくupstream `/usr/bin/sway`である。
- Sway / wlroots / Mesa / FootへPachaOS固有binary patchがない。
- `lpr_sway_launcher.c`が存在しない。
- Sway専用`LD_PRELOAD`がない。
- `LP_NUM_THREADS=0`がない。
- shader cache無効化がない。

### 操作

- keyboardでSwayの基本操作ができる。
- relative mouseでpointer / click操作ができる。
- absolute tabletでpointer / click操作ができる。
- Sway keybindからFoot、bemenu、Thunarが起動する。
- Foot上のshellを操作できる。
- Thunarでdirectoryとfileを操作できる。
- kobox USB keyboard / mouseで同じ基本操作ができる。

### SMP

- 4 CPUがonlineである。
- AP上でuser threadが実行される。
- allocator、preemption、remote wake / kill、TLB shootdownがgreen。
- 4 CPU boot 20回がgreen。

### 耐久性

- Sway normal exit 10回でresource差分0。
- Sway SIGTERM 10回でescalation 0、resource差分0。
- Sway direct SIGKILL 10回でresource差分0。
- Foot起動・終了10回でresource差分0。
- 10回目の起動が1回目より累積的に遅くならない。
- filesystem corruptionがなく、shader cache利用後のfsckがclean。

### 性能

- warm Swayが9.9秒以内に操作可能。
- first frameは実output commit / scanoutで測る。
- threaded llvmpipeが動作する。
- idle pollingがない。
- GUI操作に周期的な長時間停止がない。
- 1 CPUから4 CPUへの移行で代表的GUI workloadが実質的に改善する。

### 保守性

- fd entry、OFD、backend、service leaseの責務が分離されている。
- fd / service lifecycleにSway専用分岐がない。
- input device数、event番号、BDFが固定されていない。
- USB controller ownershipが一意である。
- wait graphとresource snapshotを取得できる。
- 正式batteryが`tests/`配下にあり、retryでfailureを隠さない。
- 一時的なdiagnostic、launcher marker、compat shimが残っていない。

以上を満たした時点で、本計画を完了とする。
