# M3.6e SMP 引き継ぎレポート (codex 作成, 2026-07-12)

M6.0 の再開用ドキュメント。WIP コードは `smp-wip` branch (f9bdde6)。「現在の作業ツリー」の記述は同 branch の内容を指す (main には M3.6d の drmd 差分のみ採用済み)。`_kobox` に変更はない。

## 1. SMP 作業の現在地

現在の kernel は QEMU `-smp 4` で BSP + AP 3 基を起動し、次のログまで到達します。

```text
boot: cpus ready count=4
```

実際に CPU 1–3 で user thread が syscall、IPC、filed/storage 処理を実行するところまで確認済みです。単に AP が idle loop に入っただけではありません。

現在到達している機能は以下です。

- Limine から ACPI RSDP と memory map を受け取る
- MADT から LAPIC ID を列挙する
- 1 MiB 未満の SIPI trampoline 領域を選ぶ
- AP 用 GDT/TSS/kernel stack/syscall entry/LAPIC timer/wake IPI を設定する
- INIT/SIPI で AP 3 基を起動する
- default scheduler の thread を CPU 0–3 に配置する
- AP 上で user mode、syscall、IPC block、idle 復帰を実行する
- `storage_boot → seed0root → filed → termd → drmd → netd` のかなり後段まで進む

最後に観測した boot probe は次の状態です。

- `boot: cpus ready count=4`: 成功
- ext4 rootfs mount: 成功
- `[filed] ready`: 成功
- termd/drmd/netd 起動: 成功
- `[seed0boot] services ready signal sent`: 成功
- `[seed0root] services ready signal received`: 成功
- その後 `/sbin/lpr_supervisor.elf` の filed exec が `-12`（ENOMEM）で失敗
- tty boot marker `[termd] linux tty hvc open ready ...`: 未到達

最後の QEMU 試験は、停止指示時点では最終 tool result を回収していません。上記はシリアルログで最後に直接確認できた状態です。

SMP 有効状態では既存の必須スモークは green になっていません。性能計測へ進める安定性にも到達していません。

## 2. 判明した事実

| 問題 | 状態 | 証拠・内容 |
|---|---|---|
| `smp.startIdleAps()` に caller がない | 解決済み | 定義は存在したが Limine boot 経路から呼ばれていなかった。現在は runtime 初期化中に呼び、`count=4` を確認 |
| Limine migration で旧 SMP boot 資源準備が失われた | 解決済み | commit `29ee24f` 以降、旧 UEFI 経路の SMP prepare/start/configure が消えていた |
| Limine RSDP を request しても利用していない | 解決済み | RSDP を kernel に渡し、RSDT/XSDT/MADT を解析 |
| SIPI trampoline 用 low memory がない | 解決済み | Limine memory map の `usable` 領域から、1 MiB 未満に CPU ごと 4 KiB を確保 |
| AP stack の identity-map が必ず失敗する | 解決済み | high-kernel VA の static AP stack に対して low identity mapper を呼んでいた。kernel image の static mapping を利用するよう変更 |
| AP の CR0/EFER が BSP と不一致 | 解決済み | QMP で BSP `CR0=0x80010013, EFER=0xd01`、旧 AP `CR0=0x80000013, EFER=0x501`。trampoline で WP/NXE を設定後、AP の CR0 が BSP と一致 |
| RDTSCP 非利用時に全 CPU が slot 0 になる | 解決済み | 物理 CPU 1 が BSP syscall stack を使っていることを QMP で確認。CPUID APIC ID と `runtime_lapic_ids` の対応で slot を求める fallback を追加 |
| CPU slot の物理/stack/scheduler 不一致 | 現在は否定 | 最後の診断では LAPIC ID、kernel stack、thread の `cpu_slot` の一致を全 AP user timer で検査し、不一致なし |
| default scheduler の AP block が即時 `false` | 解決済み | `policyActive()` のときだけ AP park していたため、通常 scheduler では `recv_wait` が即時 `-2`。通常 scheduler でも既存 AP park 経路を使うよう変更 |
| BSP block 後、CPU 0 runnable がないと block を取り消す | 部分的 | 旧コードは即時 `-2` を返した。現在は保存済み context を `loadNextReadyThread` で復帰するまで `hlt`。IPC は後段まで進むが、CPU 0 wake が周期 timer に依存する点は未完成 |
| block 復帰時に wake が書いた `rax` を上書き | 解決済み | message/fd は書かれているのに status が `-2`。保存済み context に対して通常の `switchTo(...,-2)` を再実行していた。直接 load に変更 |
| AP の非同期 user preemption | 未解決 | timer 有効時、異なる試行で user page fault、`-51/-76/-18` の異常な userland status、exec/map 失敗を観測。AP timer 無効時は fault が消えたが、scheduler starvation と BSP wake 欠落が混ざるため恒久策にはならない |
| process leader を BSP、worker のみ AP に置く案 | 撤回済み | 機構を直さず問題を踏まない妥協であり、ユーザー指摘後に全差分を revert |
| AP timer から直接 next context を load する案 | 撤回済み | park→idle→enter を減らす A/Bを行ったが、異常 status は残った。差分は revert |
| kernel interrupt 復帰の共有 `user_return_saved_r10` | 部分的 | timer/device/wake IPI の kernel-mode 復帰が全 CPU 共通 scratch を使っていた。明確なデータ競合なので、既に kernel CR3 上である復帰経路から冗長 reload と共有 scratch 利用を除去。単独では boot 安定化に至らず |
| BSP への wake IPI | 未解決 | CPU 0 の `hlt` を直接起こすため一時的に `wakeCpu(0)` を許可したが、device interrupt restore 中の kernel GP を再現。変更は revert 済み |
| absent CPU への IPI | 防御済み | `0xFF` LAPIC ID と `.absent` state を確認して送信を拒否。AP startup 失敗時も silent single-CPU fallback せず boot failure にする |
| AP thread/process の remote teardown | 未監査・未解決 | 別 CPU で実行中の thread を release/再利用する際の停止・ack が十分か未確認。principal/thread slot 再利用前の remote quiescence が必要 |
| global free page allocator の同期 | 未解決、有力候補 | `FreePageList` は lock を持たない。通常 syscall allocation は `kernel_state_lock` 下だが、page fault COW/anonymous allocation は `user_vm` lock のみで同じ `global_free_list` を操作する。最後の ENOMEM と整合するが、A/B 前に停止したため因果未確定 |
| page-table/TLB shootdown | 未監査・未解決 | 同一 address space を複数 CPU で実行する場合の mmap/munmap/COW/mprotect と remote TLB invalidate/ack は未検証 |
| kernel log の並行出力 | 未解決 | 複数 CPU の UART 出力が文字単位で交錯。`boot_log_len/buffer` にも明示的同期がない。診断精度を落としているが、現在の boot failure の支配原因とは未確定 |

## 3. 現在の作業ツリー

最後に確認した main repo の変更ファイルは以下です。

### SMP 関連

- `bootloader/limine/kernel_entry.zig`
  - RSDP/trampoline 情報を取得して kernel runtime に渡す。
  - SMP boot 資源 plumbing として概ね完成。
  - 単体では boot を壊さないが、他の SMP 差分と一体。

- `bootloader/limine/resources.zig`
  - Limine RSDP request を追加。
  - memory map から 1 MiB 未満の連続 trampoline page を選択。
  - ACPI/trampoline 資源がなければ halt。
  - SMP boot 資源取得として概ね完成。

- `kernel/src/arch/x86_64/platform.zig`
  - high-kernel static AP stack は kernel image mapping 済みなので、誤った identity-map を行わない。
  - AP 起動 blocker への最小修正として完成度は高い。

- `kernel/src/boot/entry.zig`
  - RSDP/MADT から SMP info を作り、AP syscall/timer/wake IPI を設定して `startIdleAps()` を実行。
  - `boot: cpus ready count=4` を出力。
  - SMP 起動 scaffolding は動くが、その後の scheduler/memory safety が未完成。

- `kernel/src/smp.zig`
  - ACPI RSDP/RSDT/XSDT/MADT parser。
  - INIT/SIPI trampoline。
  - CR0.WP、EFER.NXE、FX state 初期化。
  - AP state/count の atomic acquire/release。
  - LAPIC ID table。
  - fail-fast AP startup。
  - 現在、一時診断用 `currentCpuSlotFromLapicId()` が残っている。
  - AP boot 自体は動作。ファイル全体としては中途。

- `kernel/src/traps.zig`
  - RDTSCP 非利用時の CPUID APIC-ID CPU-slot fallback。
  - kernel-mode timer/device/wake IPI 復帰の共有 `user_return_saved_r10` 利用を除去。
  - 物理 LAPIC slot / stack slot / thread slot 一致の一時 invariant 診断が残っている。
  - AP preemption corruption は未解決なので中途。

- `kernel/src/scheduler_connection.zig`
  - default scheduler でも AP block を既存 park 経路へ送る。
  - BSP block は保存済み runnable context を直接 load し、無い間は `hlt`。
  - 一時診断用 `currentThreadMatchesCpuSlot()` が残っている。
  - process-leader pinning と AP preemption 無効化は revert 済み。
  - thread は現在も CPU 0–3 に通常配置され、AP timer preemption も有効。
  - block 経路は改善したが scheduler 全体は未完成。

### M3.6d から維持している drmd 差分

- `userland/drmd/src/drm_island.c`
- `userland/drmd/src/drm_island.h`
- `userland/drmd/src/drm_kms.c`
- `userland/drmd/src/drm_kms.h`
- `userland/drmd/src/main.c`

内容は 4 巡目の変更です。

- completion pump の batch 化
- pending flip / fence の不要な直列経路削減
- raw flip 約 58.9 → 169.5 fps

今回の SMP 作業とは独立しており、維持対象です。

### fixture の一時計測差分

- `userland/fixtures/src/wsl_musl/lpr_drm_page_flip_smoke.c`
  - raw flip の fill/submit/wait/read/other 時間計測。
  - 一時計測コードが残っている。最終成果には不要で revert 対象。

- `userland/fixtures/src/wsl_musl/lpr_mesa_cube_smoke.c`
  - draw/swap/KMS submit/event wait 等の計測。
  - per-frame serial `printf/fflush` も除去されている。
  - timing instrumentation は revert 対象。
  - per-frame ログ除去だけは、ログが FPS を大きく歪めることを計測済みなので、別 diff として残す余地がある。

### 文書

- `pacha_docs/refactor-plan.md`
  - 主に M3.6d の記録。
  - M3.6e の今回の SMP 調査結果はまだ正式追記していない。
  - 元からの未コミット変更として維持対象。

### `_kobox`

今回変更していません。

## 現在の build / regression 状態

現在のソース状態で最後に成功したもの:

- `cd kernel && CAPOS_UNWRAPPED_CLANG=/usr/bin/clang zig build test`
- `cd kernel && CAPOS_UNWRAPPED_CLANG=/usr/bin/clang zig build limine`
- `.artifacts/bin/pacgo build kernel`
- `.artifacts/bin/pacgo sync bootfs`

したがって、現在の tree はビルド可能です。

ただし、SMP 起動後の runtime は安定していません。

- tty boot marker 未到達
- lpr supervisor exec ENOMEM
- 過去の試行で user page fault、異常 syscall status、module map failure
- 必須スモーク未実施または未到達

よって「既存スモークを壊していない」とは言えません。むしろ現状は regression-green ではありません。

M3.6d の安定状態へ安全に戻す場合、次の SMP 関連 7 ファイルを一体で revert する必要があります。

- `bootloader/limine/kernel_entry.zig`
- `bootloader/limine/resources.zig`
- `kernel/src/arch/x86_64/platform.zig`
- `kernel/src/boot/entry.zig`
- `kernel/src/smp.zig`
- `kernel/src/traps.zig`
- `kernel/src/scheduler_connection.zig`

drmd 5 ファイルは維持します。fixture は timing instrumentation を除去します。

AP 起動部分だけを残して scheduler だけ戻す、といった部分 revert は推奨しません。AP が起動したまま single-CPU 前提の interrupt/state/allocator を使う方が危険です。

## 4. SMP 完成までの残作業

依存順では以下です。

1. allocator synchronization の一元化

   - `global_free_list` を syscall、page fault、process teardown、VMO/COW の全経路で同一 lock により保護する。
   - page fault は interrupt context なので、既存 `kernel_state_lock` をそのまま再利用すると lock order/deadlock の危険がある。
   - allocator 専用 lock または allocation API wrapper が必要。
   - 検証: 複数 CPU で mmap/COW/fork/exit/VMO create/free stress、free-page count invariant、重複 physical page 検出。

2. thread の実行所有権と remote teardown

   - 各 thread が同時に最大 1 CPU だけで active であることを明示的な state machine で保証。
   - remote kill/release 時は対象 CPU に stop IPI を送り、idle/ack 後にだけ context/principal/address space を再利用。
   - 難所は process exit と同時に複数 thread が syscall/page fault 中の場合。
   - 検証: CPU-bound thread を別 CPU から kill、process slot/thread slot の高速再利用、generation mismatch 検査。

3. AP timer preemption の再設計

   - `Running → Runnable/Blocked → Running` の遷移を atomic にする。
   - GPR、RIP/RSP/RFLAGS、FS/GS、PKRU、FX state、CR3 をどの時点で誰が所有するか明文化。
   - timer work frame と persistent thread context を混同しない。
   - next thread へ直接切り替える場合と idle loop へ戻る場合で同じ保存契約を使う。
   - 検証:
     - 全 GPR に sentinel を入れた tight loop
     - 高頻度 timer preemption
     - 4 CPU × 複数 thread
     - syscall 直前/直後の preemption
     - XMM/FP/FS/GS/PKRU preservation

4. BSP idle/wake と interrupt return

   - BSP が runnable なしで block した際、周期 timer に依存せず remote wake できる設計が必要。
   - BSP への wake IPI A/B では kernel-mode device interrupt restore 中に GP が出たため、先に kernel-mode interrupt stack/iret 契約を検証する。
   - 検証: timer を低頻度または停止して AP→BSP IPC wake、device IRQ と wake IPI の競合。

5. page table と TLB shootdown

   - address space ごとの active CPU mask。
   - mmap/munmap/mprotect/COW/exec/teardown 時の remote invalidate と ack。
   - address space を破棄する前に全 CPU がその CR3 から離れたことを保証。
   - 検証: shared address space の並行 mmap/unmap/COW、remote write/read、stale mapping 検出。

6. kernel-wide SMP audit

   - boot log / serial
   - process/fd/VMO/VMA/IPC tables
   - scheduler observer/core
   - device interrupt routing
   - `_kobox` callback が kernel global state を触る区間
   - global countersと scratch buffers

7. 段階的 regression

   - 4 CPU boot 20 回
   - native IPC ping-pong / timeout / close race
   - pthread stress
   - mmap/COW/fork/exec/exit stress
   - `drm-page-flip`
   - `kms-modeset`
   - `drm-restart 20`
   - `mesa-inventory`
   - `clang-cold-measure`
   - 最後に同一 fixture で raw/cube 内訳比較

## 5. 性能への期待値

SMP 後の raw/cube/cold 数値は一つも取得できていません。boot が安定する前に停止したため、以下は既存値からの上限推定です。

### 取得済みの SMP 前内訳

| workload | 環境 | FPS | 主な内訳 |
|---|---:|---:|---|
| raw flip | PachaOS M3.6d、ログなし | 163.2 | fill 3.627 ms / submit 1.943 / wait 0.268 / read 0.225 / other 0.063 |
| raw flip | Linux、同一 timing fixture | 2239 | fill 0.364 ms / submit 0.0175 / wait 0.0616 / read 0.0006 |
| cube | PachaOS M3.6d、ログなし | 32.8 | draw 2.30 ms / swap 23.98 / submit 4.09 / other 約0.09 |
| cube | Linux、同一 timing fixture | 約786 | draw 0.166 ms / swap 0.995 / submit 0.011 / poll 0.049 |

公式のログ付き基準値は Linux raw 2353 / cube 288、PachaOS raw 169.5 / cube 24.0 です。cube は per-frame serial log に大きく影響されるため、性能差の分析にはログなしの同一 fixture 値を使うべきです。

### cube

SMP が最も効く可能性があるのは llvmpipe worker です。ただし Linux 側でも、この fixture の `LP_NUM_THREADS=0` と default の差は概ね 1.3 倍程度でした。

- PachaOS pre-SMP: default 約32.5 fps、LP0 約35.2 fps
- Linux: default 約685 fps、LP0 約517 fps

単純に Linux の threading 比率だけを当てると PachaOS cube は 40–45 fps 程度で、60 fps を保証しません。現在の 23.98 ms swap/render wait を 16 ms 未満へ落とすには、SMP だけでなく memory allocation、scheduler wake、VM/IPC overhead の改善も必要です。

一方、現在の swap wait の一部が single-CPU scheduler の直列化だった場合は 60 fps 超えもあり得ますが、未計測です。

### raw flip

raw flip fixture 自体は主に single-threaded です。SMP だけでは 3.627 ms の buffer fill は並列化されません。

現在の fill だけでも理論上の上限は約275 fpsです。したがって「数百 fps」は、SMP で drmd/device completion を並行化した上で submit/wait を十分小さくできれば可能ですが、Linux と同オーダーの 2000 fps 級には memcpy/mapping/ioctl 差の別対策が必要です。

### clang cold / mesa inventory cold

SMP 後の測定はありません。

- clang 自体の単一コンパイルが何 CPU を使うか
- cold path が filed/ext4/VMO allocation のどれに支配されるか

をまだ再計測していないため、定量予測はできません。現時点では allocator/filed/storage の競合が安定性問題として先に出ているため、SMP 完成後に改善する可能性はあるものの、倍率は未確認です。

要約すると、SMP boot scaffolding と AP user executionまでは成立しましたが、allocator・remote lifecycle・timer preemption・TLB/interrupt の SMP 契約が未完成です。現在の tree はビルド可能ですが regression-safe ではなく、新フェーズでは性能測定より先にこれらの機構を完成させる必要があります。
