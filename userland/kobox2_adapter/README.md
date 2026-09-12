# PachaOS kobox2 foundation adapter

PachaOS native processからLinux版と同じ
Gate入りcore ELFを読み込み、upstream `start_kernel()` → SMP → initcall →
PID 1を経由して既存の第1章Gateを実行する。
LinuxのGateの合格条件・期限は変更しない。

## 接続

| 担当 | 実装 |
| --- | --- |
| RAM・image・共有alias | `image.c`: 256 MiB RAM VMO、direct/vmemmap/vmalloc予約、共有MAP_FIXED、ELF再配置、TLS |
| native thread・通知 | `native.c`, `entry.S`: thread FD、FS base、通知入口とframeの復帰 |
| freestanding補助処理 | `runtime.c`: launcher自己再配置、文字列・ログ、nativeメモリ確保、futex待機 |
| logical CPU・clockevent | `machine.c`: 共通`machine/domain.c`の所有権・IRQ状態、native timerfd/非同期通知 |
| 例外変換 | `exception.c`: native fault frameとLinux exception frameの相互変換 |
| bootfs取得 | `bootfs.c`: archive境界の検証とcore ELFの取得 |
| boot・Gate呼出し | `foundation.c`: 共通`boot/core.c`での起動、Gateごとの検証関数 |
| 第2章core側の検証入口 | `chapter2.c`: VFS／shmem／pressure／allocation／client task前提 |
| 別process VM | `vm_process.c`, `vm.c`: bootstrap配置、範囲mapping、fault通知、resume、終了確認 |
| LPR syscall transport | `vm_syscall.c`: 捕捉世代、named GPR／FP、coreへの要求と返却 |
| 停止frameの管理 | `vm_context.c`: syscall／faultのsnapshot、faultのrestore、FP検証 |
| native client | `vm_client.c`, `vm_client_entry.S`: 実アクセスと共有LPR入口。coreはリンクしない |
| VM／syscall Gate呼出し | `vm_gate.c`: 共通verifierの選択と診断、clientの破棄 |
| デバイスと資源寿命 | `device.c`, `device_pci.c`, `device_dma.c`, `device_irq.c`: PCI／MMIO／DMA／IRQ接続、inventoryとdrain |
| native IPC・起動転送 | `ipc.c`, `bootstrap.c`: FD所有権、generation、分割受信transaction |
| sandbox package入力 | `package.c`: private snapshotと共通package verifierへの接続 |

起動処理は`foundation.c`の`ph_main()`、ELFロードの手順は`image.c`の
`ph_image_open()`から読む。CPU所有権の移動は`machine.c`の`cpu_enter()`／
`cpu_switch()`、通知の流れは`cpu_notify()`／`ph_dispatch_notifications()`にある。
`host.h`はsandbox内部の宣言と状態をまとめる。native通知maskとLinuxのIRQ状態は
別管理であり、各操作の同期条件は実装箇所に記載している。

Linuxのtaskごとに実native threadを使う。協調的coroutineではない。
adapterの予約windowは所有VMAの境界・保護を記録し、nativeの単一VMA
`mprotect`へ分割する。Linux側が提供する同期の下で処理し、native失敗時は
processを停止する。複数VMA全体のtransaction成功を偽装しない。

このlauncherはboot-only試験用init process。VM／syscall／exec clientは基盤観測用fixtureで、
本番LinuxアプリをLPR以外の汎用実行環境へ移すためのものではない。
通常のservice配置とLPRのDRM／FD backendはgpud側の統合責務として分ける。
Apache-2.0 controllerはリンクしない。coreはGPL sandboxとして独立したprocessで動く。

## 再実行

repo rootから:

```sh
bash tests/run-kobox2-pacha-foundation.sh --native-primitives
bash tests/run-kobox2-pacha-foundation.sh
KOBOX_PACHA_TIMEOUT=300 bash tests/run-kobox2-pacha-foundation.sh --chapter2-core
KOBOX_PACHA_VM_CASE=readonly bash tests/run-kobox2-pacha-foundation.sh --vm
bash tests/run-kobox2-pacha-vm-suite.sh
KOBOX_PACHA_SYSCALL_CASE=rights bash tests/run-kobox2-pacha-foundation.sh --syscall
KOBOX_PACHA_CLEANUP_ROUNDS=20 KOBOX_PACHA_TIMEOUT=240 \
  bash tests/run-kobox2-pacha-foundation.sh
```

前提はLinux側で生成済みの
`.artifacts/kobox2-separated-gate-runtime/linux-boot-runtime.so`と同ディレクトリの
`linux-boot-inputs.json`、および`.artifacts/limine-boot.img`。
core生成は`kobox2/linux-sandbox/kobox/boot/build_boot_runtime.py --with-gates --link`
の既存Linux build経路を使う。adapter buildではcoreを変更しない。
clang/lld、LLVM 18のnm/readelf、Python 3、Go、Zig、mtools、QEMU/KVMが必要。

runnerは現在のkernelを`kernel/`で`zig build limine`し、64 MiBのboot imageだけを
試験用ディレクトリへ複製する。rootfsをコピー・同期・接続しない。
coreは明示manifestから独立したbootfs archiveへ格納する。
QEMUは2 CPU・2 GiB RAMで起動し、runner所有のPIDのみを終了させる。
`KOBOX_PACHA_TIMEOUT`で既定180秒の実行期限を変更できる。
`KOBOX_PACHA_CLEANUP_ROUNDS`（既定1、1〜100）は同一boot内のcleanup反復数。
全反復が成功しなければfoundation全体をPASSにしない。
同じmodeのrunnerを同時実行しない（出力先が共通）。
VM／syscallの各modeも、native client ELFの出力を共有するため同時実行しない。

出力:

- foundation: `.artifacts/tests/kobox2-pacha-foundation/`
- chapter2 core: `.artifacts/tests/kobox2-pacha-chapter2-core/`
- VM: `.artifacts/tests/kobox2-pacha-vm/`
- syscall: `.artifacts/tests/kobox2-pacha-syscall/`
- native primitives: `.artifacts/tests/native-host-primitives/boot-only/`
- 各ディレクトリの`serial.log`, `qemu.log`, `inputs.sha256`

memory/cleanup試験は意図的な保護違反を例外table経由で回復するため、serialの
`vm: fault prepare denied`自体は失敗ではない。runnerは全GateのPASSを必須とし、
FAIL/panic/予期しないprocess終了を拒否する。

`--chapter2-core`は第1章の後にcore内部の前提Gateを追加するmode。
外部VM・client実行・LPR・syscall／FD・GEM moduleの結合試験を代替しない。

`--vm-focused`／`--syscall-focused`は切り分け用。正規boot／SMPの後、
対象Gateだけを呼び、第1章・第2章全体の成功は主張しない。
出力ディレクトリには`-focused`が付き、終了markerも`PACHA_KOBOX_FOCUSED`になる。
syscall caseは`base`（既定）・`rights`・`rights-race`。
このclient frontendはLPRの既存capture entryへ入るが、zpoline decoderによる
実行コード書き換えの検証とは別である。
`run-kobox2-pacha-vm-suite.sh --focused`は同じ切り分けmodeでVMケースを巡回する。

共通coreの失敗診断は`KOBOX_PACHA_MANIFEST=tests/kobox2-vm-diagnostic.bootfs`で
別ELFを明示する。先に診断coreの生成が必要。runnerはmanifestのcore entryから
検査対象を解決し、出力先に`-diagnostic`を付ける。通常のcore ELFは置き換えない。

確認済み範囲と再実行方法は[項目5の完了記録](phase5-summary.md)を参照。
