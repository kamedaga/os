# kobox2 / gpud の接続層調査と AMDGPU 計画

調査日: 2026-09-22。現行 working tree を対象にした静的調査。
kobox2 HEAD は `b802a58`、linux-sandbox HEAD は `d8c0d0f95`。
両方とも未コミット変更を含むため、このコミットだけでは調査対象を再現できない。
実装、Linux config、kernel ABI、rootfs は変更していない。

## 結論

- 現在の Linux core は `start_kernel()` を起点に canonical built-in 全体を
  使う構成であり、Linux をユーザープロセス内で動かす点では L4Linux に近い。
  「必要な Linux subsystem の上位 API を独自実装する」という説明だけでは現状を表せない。
- ただし Linux アプリケーションの syscall 全体を同じ Linux に任せる L4Linux と違い、
  PachaOS の LPR、gpud、GPU sandbox の間で DRM 操作と資源寿命を接続している。
  この分割による追加実装は実在する。
- 「L4Linux より接続コードが明らかに多い」とは、現在の総行数だけでは断定できない。
  一方、非現行 provider の整理と DRM の多段変換には削減余地がある。
- AMDGPU は通信仕様の定義段階を越えて、実ドライバのビルド・probe・実行を
  これから接続する必要がある。中心課題は TTM の mmap、firmware、実機資源、
  DMA/IRQ/reset、そしてプロセスの mm に依存する機能である。

## 行数と比較範囲

`cloc 1.98`、拡張子 `c,h,S`、空行・コメントを除く code 行。
Python、JSON、ビルド定義、文書は含めない。ディレクトリ集計は実際のリンク集合とは違う。

| 対象 | code 行 | 解釈 |
| --- | ---: | --- |
| `kobox2/src` + `include` | 1,991 | controller / closure の小さい中核 |
| `protocol/src` + `include` | 6,138 | GPU 専用に限らない共通 protocol |
| `protocol/generated` | 5,941 | 生成されたヘッダー。手書き保守量とは分ける |
| `linux-sandbox/kobox` 全体 | 73,167 | テスト、fixture、POSIX host、provider 等を含む |
| `boot/sources.py` の COMMON + ARCH | 8,008 | 現行 core に追加する26 Cファイル。ヘッダー・sandbox launcher 等は別 |
| `userland/kobox2_adapter` | 7,930 | GPU 以外の VM / exec 等も含む |
| `userland/gpud` | 7,270 | サービス、package、DRM変換、寿命管理等 |
| `personality/linux/runtime/lpr_drm` | 2,388 | Linuxアプリ側のDRM接続 |

8,008行は接続層全体の数字ではない。逆に73,167行を実行時の接続層と呼ぶのも誤り。

比較用に公式の [L4Linux 26.07.0 snapshot](https://l4re.org/download/snapshots/)
も取得して確認した。同じ拡張子で `arch/l4` 全体は123,957行だが、
複数アーキテクチャと元のLinux由来のコードを含むため、この数字をkobox独自コードと
直接比較して優劣をつけることはできない。
範囲を絞ると `arch/l4/kernel` 直下18ファイルが6,499行、`l4lxlib` が1,577行。
これもarch別処理、ヘッダー、ドライバ、L4Re側を含まない部分集計である。
現行版の厳密な追加差分比較には、同じLinux基準への差分と同等configのリンク集合が必要。

[1997年の論文](https://www.cs.cornell.edu/courses/cs614/2003sp/papers/HHL97.pdf)
の6,500行はLinux 2.0.21への新規移植コードであり、再利用した2,000行を別にしている。
現在のSMP、DRM、サービス境界等を含むkobox全体の比較基準にはならない。

## L4Linuxとの違い

[L4Linuxの公式説明](https://l4linux.org/overview.shtml)では、LinuxをL4 APIへ移植し、
ユーザーモードで動かしてLinuxアプリケーションをその上に載せる。
通常のLinux内部のFD、mm、DRMオブジェクトの関係を保持できる。
またL4Reが提供するメモリ、DMA、capability等のAPIにも依存する。

現行gpud経路は概ね次の構成である。

```text
Linuxアプリ / Mesa
  → LPR: Linux ioctl の引数・FD・ユーザーメモリを処理
  → gpud: サービス要求をkobox GPU commandへ変換、権限・寿命を管理
  → sandbox adapter: queue/sessionとLinux呼び出しを接続
  → kobox DRM decoder / wrapper
  → 本物のLinux DRM ioctl / driver
```

返信も逆方向に変換する。共有buffer、PRIME、sync_file、mmap、process death、
sandbox generationを異なるプロセスのオブジェクトへ対応付けるため、
単なるarch portにはないコードが必要になる。
GPL sandbox、Apache controller、MIT protocolの境界も維持する必要がある。

`pacha_docs/kobox2-design.md` §10の「kernelの中で切る」という説明には、
現行 `boot/build_boot_runtime.py` のfull-core構成とのずれがある。
同様に `boot/README.md` のpatch説明よりも、現行コードのlink wrapper / compile overlayを
優先して判断した。設計文書の更新も整理作業に含めるべきである。

## 削減案

### 1. 非現行providerの整理

`provider/core_lifecycle.c` は7,517 code行あり、独自allocation、同期、thread、
timer、workqueue、RCU管理を持つ。現行 `boot/sources.py` のruntime入力には入らず、
`tests/linux/targets.cmake` ではcore lifecycle test / arena fixtureに使用される。
現行full-core経路ではLinux自身がこれらの主要な意味論を持つ。

この別経路を引き続き製品として維持する必要があるかを整理し、必要な検証を
boot-core経路へ移してから廃止すれば、千行単位で保守対象を減らせる。
ただしこれは実行時経路の短縮ではない。`provider`という名前のbuild入力や
ビルド用Pythonは現行構成にも使われるので、ディレクトリごとの削除は不適切。

### 2. DRMの手書き変換を集約

以下5ファイルだけで6,191 code行ある。

| ファイル | code 行 |
| --- | ---: |
| LPR `lpr_drm/client.c` | 2,342 |
| gpud `drm_translate.c` | 1,197 |
| gpud `drm_reply.c` | 572 |
| adapter `gpu_query.c` | 430 |
| sandbox `boot/drm_query.c` | 1,650 |

この全量が重複ではないが、同じcommandのfield、count、span、入出力方向を
複数箇所で記述している。まず固定長queryと可変長queryを各1つ選び、
既存schemaから変換記述を生成する試作で、正味削減量を測る。
generator追加分・schema追加分も保守量に含め、生成コード増加と分けて報告する。

次に、LPRがcanonicalなpointer-free commandを作り、gpudは資源・session・権限・
generationを検証して中継、sandboxがLinux UAPIへ復元する形を検討する。
これによりgpud固有の中間DRM表現と変換往復を減らせる可能性がある。
AMDGPU20コマンドを現行の各層へ手作業で追加する前に決めたい。

共有メモリのbounds/権限確認、各信頼境界での検証、入力snapshot、FD/token変換、
PRIMEやmapping leaseの寿命は維持する。同一の検証コードを生成することと、
独立した境界の検証を省くことは別である。

中間表現をなくす案はLPR↔gpudサービス契約の変更を伴い得る。
目的は重複表現の排除であり、Linux pointerやhost FDをwireへ流すことではない。
`dev` interfaceとして送受信側を同時更新する。Pacha kernel ABI変更は前提にしない。

### 3. Linuxへの依存を下位境界へ集中

現行のfull-core方針を維持し、AMDGPUのためにTTM、scheduler、firmware APIを
独自に再実装しない。Linux側は `linux-sandbox/kobox` のmachine/resource/mapping境界を
補い、本物のLinux実装を使う。

Linuxアプリ全体をsandbox内のLinuxプロセスへ移せばDRM転送の多くをなくせるが、
Pachaのサービス構成・隔離・process/FD管理を変える大きな設計変更になる。
今回の削減案の前提にはしない。

## 現在のAMDGPU到達点と不足

- `protocol/schema/gpu_drm_amdgpu.json` に20コマンドとnested span等の契約がある。
  schemaの存在は実行対応を意味しない。
- `gpud/package.c` の13 artifactはvirtio-gpu closure。
  `gpud/build-package.sh` も `--virtio-gpu` を使う。
- 調査時のcanonical/provider `.config` は共に `DRM_AMDGPU` と `FW_LOADER` が無効。
- gpud/LPRの実行変換とadapterのsymbol bindingはvirtgpu中心で、
  AMDGPU commandを実ドライバへdispatchする経路を確認できない。
- `gem/drm_mapping.c` はdriver名 `virtio_gpu`、GEM shmemのvm_ops、既存sgt/pagesを要求し、
  imported GEMとWCを拒否する。AMDGPUの一般的なTTM mmapには流用できない。
- `boot/drm_file.c` / `drm_service.c` は `current->mm == NULL` のownerを要求する。
  AMDGPUのHMM/userptrは `current->mm` とmmu notifierを使うので、
  単純な「ユーザーアドレスをbufferへコピー」で同じ意味論にはならない。
- 既存PCI/DMA/IRQ、MMIO cache属性、syncobj/fence、generationの基盤は再利用候補。
  実機でのROM/BIOS取得、reset、BAR aperture、DMA rangeの成立は別途検証が必要。

## 推奨実施順序と完了条件

対象GPUは既存設計のRX 9060 XTを前提にする。実ボード、VRAM容量、PCI ID、
CPU/chipset、接続先が変わればprofileを見直す。

| 段階 | 作業 | 次へ進む条件 |
| --- | --- | --- |
| A: 接続層整理 | 実リンク集合の棚卸し、providerの用途確定、DRM変換集約の試作 | 正味の手書き削減を計測。既存VirGLの動作・寿命試験が維持される |
| B: AMD profile | Linux/config/compiler/firmware/Mesa/libdrmを固定し、AMDGPU/TTM/DRM scheduler/DC等をKconfigから構築 | strict link・modpost・module dependencyとinitcallが成立。成功stubなし |
| C: 実機probe | capabilityによるdevice選択、BAR/ROM/BIOS、firmware、MSI、DMA、VRAM/GTT、ring初期化 | probe成功、render node、ring testとIRQ完了、確実な停止 |
| D: メモリとrender | TTM mmap/fault、cache属性、mapping撤回、GEM/VA/CS/fenceを接続 | GPU copy/render結果一致、CPU↔GPU整合、memory pressure/eviction中もmappingが正しい |
| E: Mesaと表示 | 必要なquery/CS/sync/PRIME/KMSを接続し、radeonsiとXorg/Xfceを試験 | 実AMD rendererで描画・page flip・resize。CPU rendererへのfallbackなし |
| F: 障害と拡張 | GPU hang/reset、process death、sandbox restart、userptr/HMM、user queue等 | 古いmapping/IRQ/DMAの再利用なし。対応profileの機能を実測して公開 |

BではupstreamのKconfig/依存関係からclosureを導く。AMDGPU Kconfigは
FW_LOADER、DRM_TTM、DRM_SCHED、DRM_EXEC、DRM_BUDDY等を選択する。
firmwareは固定したstoreからLinux標準loaderが読めるよう、sandbox内VFSへの配置等を
kobox側で行う。実際に要求されるfirmware名とhashを記録する。

Dが最大の設計課題。LinuxのTTM mmap/faultと移動時のmapping失効を維持し、
CPU側aliasが移動前ページを参照し続けない仕組みにする。
WC/UC属性とVRAM apertureを扱う必要がある。
shmem方式の恒久pinやコピーだけでAMDGPU全体の完成とはしない。

userptr/HMMは通常BOによるrenderとは別の完了条件を置く。
LPRのmmに対応するLinux mm/VMA、page pin、invalidate通知が必要になる。
未接続機能は明示的に拒否し、20コマンド全部が動くとは宣言しない。
DRM user queueにもdoorbell、メモリ権限、queue停止の別の検証が必要。

AMD GPUとAMD CPU/chipsetは分けて考える。現状確認できるhost IOMMUは
Intel VT-d系であり、AMDプラットフォームならIVRS/AMD IOMMU対応の有無を
開始時に確認する。GPUメーカーだけを理由にAMD IOMMUの実装を追加しない。

kernel側の変更が必要になった場合は、例えばcache属性付きmappingの委譲・撤回など、
既存capability APIで成立しない具体例を示し、userlandで解決できない理由を説明して
個別に許可を得る。Linux/OSS本体へのpatchを前提にせず、kobox側で実装する。

## 今回の検証

以下の既存テストを実行し、すべてPASS。

- `tests/run-gpud-drm-translate-unit.sh` — canonical command変換。service実行試験ではない。
- `tests/run-gpud-drm-fence-unit.sh` — 非同期fenceのidentity、close/error、replay、寿命。
- `tests/run-kobox-drm-memory-policy-unit.sh` — メモリ予約・境界・overflow。

今回QEMU描画とAMDGPU実機は実行していない。
今後のVirGL回帰ではhost rendererをD3D12に固定し、LLVMPIPEを合格にしない。
AMDGPUの合格判定は実AMD GPUで行い、VirGLの合格で代用しない。
