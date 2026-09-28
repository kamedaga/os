# kobox2 / gpud の接続層調査と AMDGPU 計画

初回調査日: 2026-09-22。以下の行数・接続層比較は当時の working tree の静的調査。
kobox2 HEAD は `b802a58`、linux-sandbox HEAD は `d8c0d0f95`。
両方とも未コミット変更を含むため、このコミットだけでは調査対象を再現できない。
この時点では実装、Linux config、kernel ABI、rootfs は変更していない。

2026-09-26更新: 実機では AMD IOMMU を通して GOP live・USB HID・r8169・DHCP・
HTTPS・公開鍵 SSH が動作した。以下のAMDGPU進捗と実施順序はこの更新を優先する。
今回の作業は独立した任意起動profileであり、通常live USB・通常gpud・自動probeは変更しない。

## 結論

- 現在の Linux core は `start_kernel()` を起点に canonical built-in 全体を
  使う構成であり、Linux をユーザープロセス内で動かす点では L4Linux に近い。
  「必要な Linux subsystem の上位 API を独自実装する」という説明だけでは現状を表せない。
- ただし Linux アプリケーションの syscall 全体を同じ Linux に任せる L4Linux と違い、
  PachaOS の LPR、gpud、GPU sandbox の間で DRM 操作と資源寿命を接続している。
  この分割による追加実装は実在する。
- 「L4Linux より接続コードが明らかに多い」とは、現在の総行数だけでは断定できない。
  一方、非現行 provider の整理と DRM の多段変換には削減余地がある。
- AMDGPU は独立profileでLinux標準ビルド、module closure抽出、Kobox2 hosted runtimeと
  19モジュールの厳密リンクまで到達した。module load・probe・実行は未接続。
  中心課題は TTM の mmap、firmware、実機資源、
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

この旧core providerと専用fixtureは2026-09-23に整理した。対応関係は次の通り。
「同じ旧provider APIを再試験した」という意味ではなく、現行Linux coreが必要とする
機能・失敗処理をどのテストが担うかを示す。

| 旧fixtureが扱ったもの | 現行の確認先 |
| --- | --- |
| page/heap/cache/per-CPU allocation | boot-coreのmemory gate、allocation gate |
| thread、同期、待機、timer | task/SMP gate、wait gate、cleanup gate |
| workqueueとRCUの実行・終了 | workqueue gate、RCU gate、cleanup gate |
| closureのload、init失敗時の巻き戻し、資源解放 | `kobox2.closure_loader`、`kobox2.resource_import_rollback` |
| 旧coreの未解放arenaでclose失敗 | 旧provider固有の検査。現行coreで同じ戻り値を持つとは主張しない。実Linux moduleの解放はmodule gateが確認する |

削除対象は `core_lifecycle.c`、そのテスト、`core_arena_consumer`、専用closure
fixtureとCMakeターゲット、および未ビルドの旧core adapter/closure testに限定した。
共用の`provider/arena.c`、`provider/lifecycle.c`、ビルド用Python、共通protocolは
残した。これは実行時経路の短縮ではなく、使われない別実装の保守負担削減である。
整理後、共用providerのarena/lifecycle、closure loader、resource import rollback、
boot runtime buildの単体テストと、gate有効のboot coreによる
`kobox2.linux_full_foundation_gate`を通した。旧arena leak fixture固有の戻り値は
現行経路への同等移植を主張しない。

### 2. DRMの手書き変換を集約

以下5ファイルだけで6,191 code行ある。

| ファイル | code 行 |
| --- | ---: |
| LPR `lpr_drm/client.c` | 2,342 |
| gpud `drm_translate.c` | 1,197 |
| gpud `drm_reply.c` | 572 |
| adapter `gpu_query.c` | 430 |
| sandbox `boot/drm_query.c` | 1,650 |

この全量が重複ではない。各層に必要な権限・bounds確認、入力snapshot、
FD/token変換、PRIMEやmapping leaseの寿命管理は維持する。

まず固定長queryを1つ選び、**同一ファイル内**の繰り返しだけを既存のhelperへ
寄せられるか確認する。小さな変更でも読みやすくなり、変換・返信・エラーが
既存テストで一致する場合に限って適用する。次に可変長queryを1つ調べ、
共通化で分岐や状態が増えるなら変更しない。

schemaからの新しいgenerator、LPR↔gpudの通信形式変更、AMDGPU20コマンドを
見越した汎用dispatch基盤は今回の整理には含めない。AMDGPU実装で同じ記述が
実際に増えた時に、必要な最小単位で再検討する。

2026-09-23の確認では、固定長の`GET_CAP`返信を選んだ。sandboxの
`boot/drm_query.c`で64-bit値を`write_u32`二回で書く箇所だけを、同じファイルに
既存の`write_u64`へ寄せた。変換と返信の64-bit値全体、失敗時のエラー・無変更を
検査する`run-gpud-gpu-query-unit.sh`は変更前後とも通過した。
可変長の`GET_RESOURCES`は、同ファイル内ですでに`output_span`がcount、
record size、boundsを検証している。隣接する`GET_CONNECTOR`とはrecord種別、
force-probe条件、返信時の不足容量の扱いが異なるため、両者をまとめるための
追加分岐や状態は作らなかった。
変更を含む製品用boot coreとDRM moduleを再ビルドし、QEMUのMesa multi-client
smokeでcard0・renderD128とも`virgl (D3D12 (Intel(R) Graphics))`、3 clientの
描画・共有・再接続完了を確認した。boot profile切替の既存生成スクリプトは検証中だけ
一時調整し、テスト後に原状へ戻した。LLVMPIPEは合格判定に使っていない。

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
- 2026-09-22調査時のcanonical/provider `.config` は共に `DRM_AMDGPU` と `FW_LOADER` が無効。
  2026-09-26に別の `amdgpu` profileを追加し、`DRM_AMDGPU=m`、DC、TTM、DRM scheduler、
  firmware loaderをKconfigで選択した。Linux標準の `vmlinux modules`、modpost、
  boot-core inventory、19モジュールのclosure inventory、Kobox2 hosted runtimeと
  hosted modulesの厳密リンクは通過。module load成功を意味しない。
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

### gpudをGPU共通の所有者にする

`gpud`はvirtio-gpu専用デーモンを別に残したままAMDGPU用デーモンを増やすのではなく、
許可されたGPUのPCI capability、Kobox2 closure、sandbox generation、DRM公開と
失敗時の撤回を一元管理するGPU共通サービスにする。ドライバのPCI ID、Linux module名、
firmware名を`gpud/main.c`や`seed0root`の分岐に追加しない。既存のvirtio-gpu経路は
移行中も動作を維持する。

- GPU packageはKobox2 closure inventoryから生成するmanifest、core、module、
  sandbox、必要な場合だけ固定hashのfirmwareで構成する。`gpud`はmanifestと実ファイルの
  size/hashを照合してから渡す。現行の`gpud/package.c`にあるvirtio固定のartifact表・
  dependency表は、実際に使うpackage descriptorへ移し、AMDGPU分を同じ表へ追記しない。
- 共通の起動・権限・監督経路と、提供可能なDRM操作を分離する。virtio-gpuの専用queue／
  command変換は対応backendに閉じ込める。AMDGPUのprobe段階ではrender nodeを確認しても
  virtio command endpointを公開しない。未実装のDRM操作は成功扱いにせず明示的に拒否する。
- GPUの選択はboot側にvendor/device ID表を置かず、渡されたPCI capabilityとLinuxの
  実際のdriver bindで確定する。AMDGPU packageの起動は別の明示的な試験経路だけにし、
  通常liveのbootfs・自動probe・GOP表示を黙って置き換えない。
- firmwareはGPU package側でhashを固定し、sandbox内のLinux標準`request_firmware()`が
  読めるようにする。要求名、成功／失敗、size、hash、供給元をRAMログに記録する。
  候補15個を実機で要求される全件とみなさず、欠けた要求は失敗として記録する。

最初の実装単位は、既存virtio-gpuの回帰試験を保ちながらpackage読込と共通監督を
driver非依存にし、AMDGPUではPCIなしのfirmware／module／失敗cleanup試験まで通すこと。
その後、SSHとRAMログが使える別の試験imageでのみ実機probeを許可する。
「汎用gpud」は全GPUに同じDRM ioctlを無条件で通すことを意味しない。

対象GPUは既存設計のRX 9060 XTを前提にする。実ボード、VRAM容量、PCI ID、
CPU/chipset、接続先が変わればprofileを見直す。

| 段階 | 作業 | 次へ進む条件 |
| --- | --- | --- |
| A: 接続層整理 | 旧providerの参照とテスト対応を確認し、重複fixtureを段階的に整理。DRMは既存ファイル内の明白な重複だけを対象にする | 行数だけでなく読みやすさと検証範囲を確認。既存VirGLの動作・寿命試験が維持される |
| B0: build-only profile（完了） | pinned Linux/config/compilerでAMDGPU/TTM/DRM scheduler/DCを別ビルド | Linux標準ビルド・modpost・inventory、Kobox2 hosted coreと19モジュールのstrict link成立。通常live経路への影響なし |
| B1: Kobox2実行準備（進行中） | 許可されたPCI機能配下のDRMノードを共通判定にし、virtioコマンドサービスはAMDGPUに公開しない。firmware格納・読取経路を接続 | 制御されたload/unload、実際に要求されたfirmwareのhash・供給元を記録。成功stubなし |
| C: 実機probe | capabilityによるdevice選択、BAR/ROM/BIOS、firmware、MSI、DMA、VRAM/GTT、ring初期化 | probe成功、render node、ring testとIRQ完了、確実な停止 |
| D: メモリとrender | TTM mmap/fault、cache属性、mapping撤回、GEM/VA/CS/fenceを接続 | GPU copy/render結果一致、CPU↔GPU整合、memory pressure/eviction中もmappingが正しい |
| E: Mesaと表示 | 必要なquery/CS/sync/PRIME/KMSを接続し、radeonsiとXorg/Xfceを試験 | 実AMD rendererで描画・page flip・resize。CPU rendererへのfallbackなし |
| F: 障害と拡張 | GPU hang/reset、process death、sandbox restart、userptr/HMM、user queue等 | 古いmapping/IRQ/DMAの再利用なし。対応profileの機能を実測して公開 |

B0は `pack/scripts/build_amdgpu_profile.sh` で再実行できる。upstream Kconfigから
FW_LOADER、DRM_TTM、DRM_SCHED、DRM_EXEC、DRM_BUDDY等が選ばれた。
`modinfo` が宣言するfirmware 583件は全ASIC向けの上限リストで、RX 9060 XTに必要な
実集合とは異なる。B1で実機のIP blockに対応するfirmwareを固定したstoreから
Linux標準loaderへ提供し、実際の要求名・hash・供給元を記録する。
Mesa/libdrmの固定と実描画はEで行い、B0のビルド成功を描画成功とは扱わない。

B1の最初の検査として、`pack/scripts/test_amdgpu_modules.sh` がPCI grantを一切
渡さずにhosted Linuxで19モジュールをロード／逆順アンロードし、
`loaded=19 unloaded=19 cleanup=0` を確認した。これはGPU probeの成功ではない。
DRM discoveryはPCI driver名やvendor IDに依存せず、許可されたPCI機能の子孫に
限定した。一方、既存のコマンドサービスはvirtio-gpu専用なのでAMDGPU profileでは
公開しない。RX 9060 XT向け候補15個はlinux-firmware `20260916` のコミットを固定し、
`amdgpu-firmware.sha256` と `pack/scripts/fetch_amdgpu_firmware.sh` でローカル取得・
hash検証まで行った。後続のB1前半ではLinux標準loaderのPCIなし読取まで通したが、
実機で要求される正確な集合は未確定。後続のB1作業で`gpud`からの供給経路を
実装・ビルドしたが、実機経路では未検証。通常bootfs/USBには混入させていない。

Dが最大の設計課題。LinuxのTTM mmap/faultと移動時のmapping失効を維持し、
CPU側aliasが移動前ページを参照し続けない仕組みにする。
WC/UC属性とVRAM apertureを扱う必要がある。
shmem方式の恒久pinやコピーだけでAMDGPU全体の完成とはしない。

userptr/HMMは通常BOによるrenderとは別の完了条件を置く。
LPRのmmに対応するLinux mm/VMA、page pin、invalidate通知が必要になる。
未接続機能は明示的に拒否し、20コマンド全部が動くとは宣言しない。
DRM user queueにもdoorbell、メモリ権限、queue停止の別の検証が必要。

実機はRyzen 9 5900X / B550 / RX 9060 XTで、IVRS解析、AMD IOMMU初期化、
USB・r8169等のtranslated DMAまで成立した。GPU自体のDMA範囲、BAR、MSI、
resetを安全に扱えるかはまだ未検証。既存のAMD IOMMU動作をGPU成功の証明にしない。

kernel側の変更が必要になった場合は、例えばcache属性付きmappingの委譲・撤回など、
既存capability APIで成立しない具体例を示し、userlandで解決できない理由を説明して
個別に許可を得る。Linux/OSS本体へのpatchを前提にせず、kobox側で実装する。

## 2026-09-22の検証

以下の既存テストを実行し、すべてPASS。

- `tests/run-gpud-drm-translate-unit.sh` — canonical command変換。service実行試験ではない。
- `tests/run-gpud-drm-fence-unit.sh` — 非同期fenceのidentity、close/error、replay、寿命。
- `tests/run-kobox-drm-memory-policy-unit.sh` — メモリ予約・境界・overflow。

今回QEMU描画とAMDGPU実機は実行していない。
今後のVirGL回帰ではhost rendererをD3D12に固定し、LLVMPIPEを合格にしない。
AMDGPUの合格判定は実AMD GPUで行い、VirGLの合格で代用しない。

## 2026-09-26のB0検証

- `amdgpu.config` を task/boot config に重ね、固定Linux 6.18.48+・Clang/LLD
  18.1.3 で `vmlinux modules` をビルド。upstream Linux sourceは変更なし。
- Linux標準 modpost で `amdgpu.ko` と依存moduleを生成。
  boot-core inventoryは `start_kernel` とinitcall境界を検証し、
  closure inventoryは19モジュール・依存順序・1859 importを検証した。
- virtio専用のGPU serviceを含まないPCI/module境界でKobox2 hosted coreを厳密リンクし、
  同一profileの19モジュールをKobox2 hosted objectとしてmodpost・リンクした。
- `modinfo` に宣言されたfirmware 583件を `.artifacts/kobox2-amdgpu-profile/`
  に採録。これは実機用firmwareの選定・取得・hash固定ではない。
- Kobox2 hosted runtimeの実行とmodule load、RX 9060 XT probe、ring/IRQ、描画は未試験。
  `device_launch.c`の共通PCI DRM判定だけではfirmware供給とgpudの非virtio起動経路が
  まだ成立しないため、AMDGPU実機起動の証明とはみなさない。

## 2026-09-26のB1前半検証

- AMDGPU専用Linux profileだけで`CONFIG_FW_LOADER_DEBUG`と動的debugを有効化。
  通常live profileとbootfsには加えていない。実機でloaderの成功名・SHAを出すには
  `firmware_class`に`dyndbg=+p`を渡す必要があり、AMDGPU専用sandboxはこのmoduleを
  ちょうど1件要求して起動パラメータを付ける。これはLinux標準loaderの要求ログであり、
  staged fileの一覧を実際の要求と取り違えないための設定である。
- `kobox/boot/firmware_files.c`は、受け取った各データのSHA-256をLinux側で照合し、
  private rootfsの`/lib/firmware`へ排他的に配置する。module lifecycle終了時に逆順回収。
  `filp_close`の遅延fputではLinux loaderが`ETXTBSY`を返したため、ここだけ同期fputを使う。
- `pack/scripts/build_amdgpu_package.sh`は通常liveと別の`.artifacts`に、core、19 module、
  AMDGPU専用sandbox、Kobox2 manifest、固定SHAのfirmware候補15件と読み取り専用
  `firmware.bundle`を出力する。bundle全体のSHAと各fileのSHA、名前、範囲、重複を
  sandbox側で検査してからLinuxに渡す。正常・改ざん・`..`・範囲重複は
  `pack/scripts/test_amdgpu_firmware_bundle.sh`で単体試験した。
  packageは`gpud`のmanifest読込経路に接続し、後続作業で手動probe入口と専用live imageを追加した。
- PCIを渡さない`pack/scripts/test_amdgpu_modules.sh`でLinux標準`request_firmware()`を
  15件実行し、各要求名・結果・SHAをLinuxのログとKobox試験moduleのログで照合。
  正常系は`loaded=20 unloaded=20 cleanup=0`でfirmware 15件も回収した。
  SHA不一致は配置前に`-EBADMSG`で拒否し`loaded=0`。存在しない要求は`-ENOENT`で
  `loaded=19 unloaded=19 cleanup=0`となり、配置した15件も全て回収した。

固定SHAを`gpud`起動設定からsandboxへ渡す経路、virtio専用RPCを閉じ込める共通監督経路、
Linux firmware・AMDGPUのRAMログ転送は実装・ビルドした。後続作業でboot後にGPU
capabilityを渡す制御経路も追加したが、実機probeと停止動作は未検証である。
render node確認だけをring/IRQ合格とはしない。

bundleの受け渡しに備えて、同時ビルドされるlauncher/sandbox専用の
`ph_sandbox_config`をversion 3に上げた。これは公開kernel ABIではなく、
読み取り専用bundleのアドレス・サイズ・SHAと、任意のSEND専用RAMログcapabilityを
渡すためのprivate起動契約である。通常virtio GPU、USB、networkはこれらを零で渡し、
AMDGPU sandboxだけがbundleを必須とする。`gpud_boot_config`もversion 6とし、
PCI IDではないbackend種別・package root・期待firmware/manifest/sandbox SHA・任意のログcapabilityを持つ。同時に
`seed0root`側の通常virtio起動設定を更新した。旧版とのABI互換は維持しない。

`gpud`のpackage/controller層はmanifestからartifact数、依存、root、PCI bindingを
構成するように変更した。通常virtio packageは従来の固定manifest生成を互換経路に
残し、別packageは`/srv/kobox2/<name>/manifest.bin`から読み、artifact bytesを
manifest SHAに照合する。AMDGPU bundle全体の期待SHAはpackage自身のファイルではなく
owner側の`pack/live-amdgpu-probe.conf`に固定して起動設定で`gpud`へ渡し、
build時にも一致を検査する。検査backendはvirtio queue/RPCを
公開せず、停止要求時にLinux moduleをunloadし、controllerによるDMA revokeまで進める。
ただしこの新規経路は実機での起動・停止を未検証である。従ってこの時点で安全停止が
成立したとは言わない。

## 任意の実機probe経路（2026-09-26）

別の`pack/live-amdgpu-probe.yaml`を指定したlive imageだけが、`gpud.elf`と
`/etc/pacha/gpu-probe.conf`を追加する。通常bootfsにはGPU moduleもfirmwareも入れない。
このpolicyはboot imageに固定されたfirmware bundle、manifest、sandboxのSHAを持つ。
GPU packageはSSH確立後にRAMへ転送する。`seed0root`はPCI IDを判定せず、曖昧さのない
display-class PCI capabilityだけを渡す。SSH公開鍵、networkサービス、RAM logd、
IOMMU translated DMAが揃わなければoptional gpudを起動せず、live shellを続ける。
live RAMルートは任意の約40 MiB GPU packageも保持できるよう、tmpfsの総ページ上限を
64 MiBから128 MiBへ拡張した。初期bootfsの展開上限64 MiBとは別で、ページは書き込み時に確保する。
`gpud`は起動直後にprobeせず、root所有の`/run/pacha/gpu-control`に完全な
`probe\n`が書かれるまで待つ。このRAMファイルは開始合図であり、PCIを選択したり
SHA検証を省略したりする権限ではない。

ビルド済みの`.artifacts/live-amdgpu-probe/limine-boot.img`から起動した後、SSHで
`/run/pacha/service.log`の`armed`行を確認する。実機のIPとSSH鍵を引数で渡し、
packageをRAMへ分割転送して各ファイルのSHAを実機で照合する。`--probe`を付けなければ
GPUには触れない。転送先のパスはこの任意profile専用で、通常rootfsへのコピーはしない。

```sh
bash pack/scripts/upload_amdgpu_probe.sh \
  root@<DHCPで得たIP> "$HOME/.ssh/pachaos_live_ed25519"
# ログとhashを確認してから、明示的に一度だけ起動する場合:
bash pack/scripts/upload_amdgpu_probe.sh \
  root@<DHCPで得たIP> "$HOME/.ssh/pachaos_live_ed25519" \
  .artifacts/kobox2-amdgpu-package --probe
```

更新版の専用live imageでは、`gpud`が完全な`probe\n`を受けてから
`/run/pacha/gpu-probe.conf`を読み、同じboot-granted PCI capabilityと
`/srv/kobox2/amdgpu`のまま、RAMで指定されたfirmware/manifest/sandboxの固定SHAを
使う。誤った形式・hash・別のpackage rootは拒否する。再起動するとRAMのpackageと
policyは消えるが、USBはサブPCに挿したまま同じコマンドで再転送できる。
`gpud.elf`やseed0root等、bootfs自体を変える時だけ媒体更新が必要で、Kobox2 core・
module・sandbox・firmwareの診断サイクルでは不要。失敗後の同一bootでの再probeは
自動化しない。GPUのresetが保証できないため、再試験は再起動後に行う。
現行のlive SSHホスト鍵はRAMで起動ごとに生成される。転送スクリプトは同じ実行中の
接続間だけ鍵を固定するTOFU方式であり、恒久的なサーバー認証は未達。

`busybox tail -n 100 /run/pacha/service.log`でLinux module、firmwareの要求名と結果、
失敗元・errno、DMA隔離、停止結果を読む。成功した場合もring/IRQ・render nodeの
実証は別途必要。停止要求は`busybox printf 'stop\n' > /run/pacha/gpu-control`。
応答しない転送とprobe readyは各180秒で失敗扱いにしてDMAを隔離する設計だが、実機での
停止・resetは未確認なので初回は遠隔画面消失と電源断の可能性を前提にする。

### 実機の短時間温度監視（2026-09-27）

AMDGPU inspection専用のKobox2では、render nodeを開いている間に1秒ごとに
`AMDGPU_INFO_SENSOR_GPU_TEMP`を読み、取得できれば平均電力もRAMログへ記録する。
最初の2.5秒だけを検査して待機中を放置する旧方式は使わない。センサー取得失敗、
70°C到達、または開始から120秒で検査を終了して通常のmodule cleanupへ進む。
70°Cはこの短い試験の中断基準であり、RX 9060 XTの公称温度限界を意味しない。
Linux内部の終了要求はnative受信スレッドも解放してからsandboxを退場させる。

実機では`upload_amdgpu_probe.sh --probe`を単独使用せず、監視役のメインPCで
次を実行する。接続先と秘密鍵は明示的な引数であり、IPや機種を通常経路へ固定しない。
ホスト側もprobe開始から150秒を独立した期限とし、停止を要求して最大12秒だけ
停止ログを待ち、直ちに`busybox poweroff -f`を要求する。SSHが切れた場合には
物理的に電源が切れたことを確認し、切れていなければ手元で電源を落とす。

```sh
bash pack/scripts/run_amdgpu_inspection.sh \
  root@<DHCPで得たIP> "$HOME/.ssh/pachaos_live_ed25519"
```

センサー値はGPU内部の値であり、背面端子や基板の温度を示さない。またLinuxの
`stop`成功はGPU電源断を保証しないので、この試験ではstop後に通電したまま放置しない。

2026-09-27の2分間試験では、GPU内部温度は51°Cから42°Cへ下がった。
当初は`GPU_INPUT_POWER`を選んだため電力取得が`-EINVAL`だったが、
SMU 14.0.2は`GPU_AVG_POWER`を実装している。次回用のKobox2経路を修正したが、
この日の実機で修正後の電力取得はまだ未検証。2分上限でsandbox停止ログを確認後、
ACPI電源オフを要求した。続くpingとSSHは応答せず、物理電源状態は別途確認が必要。

2026-09-26の実機probeでは、15 firmwareのstagingと19 moduleのloadまで進んだ。
PCI grantとLinux BARは、BAR0 `0xd0000000/256 MiB`、BAR2
`0xe0000000/2 MiB`、BAR5 `0xfce00000/512 KiB`で一致した。
AMDGPUのregister BAR5 `ioremap`で、Kobox2のhost MMIO overlay公開が
`status=-5`を返した。その結果Linuxの`ioremap`がNULLになり、driver probeは
`-ENOMEM`で失敗。続く`iounmap`のUD2は後始末中の二次faultで、直接原因と
混同しない。gpudのDMA遮断とsandbox停止は共に成功したが、物理GPU resetではない。
BARの列挙・サイズとfirmware配置は今回の失敗点ではない。host adapterでは
`PACHA_SYSCALL_ERR_MAP`が`-EIO`に変換され、この戻り値だけではkernel側の
cache/MTRR確認、virgin PROT_NONE予約確認、overlay PTE公開等を区別できない。
冷間起動後にUSBを更新せずLANから診断入りパッケージを転送し、1回だけ再probeした。
同じ失敗が再現し、拒否された要求はVA `0x71c00000`、PA `0xfce00000`、
長さ`524288`、cache `UC_MINUS`、保護`READ|WRITE`、host結果`-EIO`と判明した。
要求VAはKobox2の予約済みvmalloc window `0x70000000..0x80000000`内に収まる。
これだけではkernelのcache/MTRR、virgin予約、PTE公開のどれが失敗したかは
確定しない。同一bootでの再probeは禁止し、物理GPU resetを仮定しない。

2026-09-26追記: `DERIVE_MMIO` の UC_MINUS cache拒否はkernelだけがCPU間の
PAT/MTRR証拠を持つため、既存の一律`MAP`から意味に沿う戻り値へ分類した。
証拠が成立していない場合は`NOT_READY`（adapterでは`-EBUSY`）、証拠が
成立しても要求範囲が安全でない場合と未対応cacheは`INVALID`（`-EINVAL`）。
予約・PTE公開の失敗は従来の`MAP`（`-EIO`）のまま。番号・構造は変えないが、
既存呼出しの失敗時意味が変わるABI変更である。UC成功経路とcache許可基準は
変えていない。`NOT_READY`には恒久的なCPU間証拠不一致も含まれるため、
gpudは自動再probeしない。kernel unit、Kobox2 PCI adapter unitで分類を検証した。
更新kernelでの一回限りの実機probeでも `-EIO` だったため、cache拒否は除外された。
この時点の`MAP`はaddress space取得・未使用予約の確認・PTE公開を区別できない。

続いてkernelにしか見えない状態を既存の戻り値で分類した。address spaceが
取得できない場合は`NOT_READY`、要求VAが未使用の匿名PRIVATE|NORESERVE|PROT_NONE
予約でない場合は`INVALID`、PTE公開失敗は従来どおり`MAP`とする。Kobox2 adapter
ではそれぞれ`-EBUSY`、`-EINVAL`、`-EIO`となってRAMログに出る。ABIの番号・構造は
変わらないが、これも失敗時の戻り値意味変更である。これは観測のための分類であり、
cache属性の緩和、予約の強制上書き、失敗後の自動再probeはしない。全256 MiB
vmalloc予約を含むkernel unitでも、対象512 KiBの未使用範囲は受理できる。
ただし実機sandboxの実際のVMA状態まではこのunitで証明しない。

さらに静的に、Kobox2の低位vmalloc窓 `0x70000000..0x80000000` はkernelの
低位16 GiB supervisor identity領域内と判明した。user CR3に継承される
identity PTEは、VMAが未使用予約でも`present`である。通常のuser mappingは
supervisor seedを置換できる一方、従来のMMIO overlayは全present PTEを拒否し、
今回のBAR5 VA `0x71c00000`で`MAP`となることを説明できる。これはGPU固有の
回避策ではなくkernelの低位VMA/PTE整合性の問題である。kernelだけがPTEを
検証し同期TLB shootdownできるため、userlandでの予約し直しでは解決しない。
overlayは、呼出し元が未使用の匿名PRIVATE|NORESERVE|PROT_NONE VMAを証明した上で、
各PTEが未存在か、物理アドレスがVAに一致しフラグがseed形式のsupervisor
identity PTEである場合に限り受理する。既存user PTEや他のsupervisor PTEは
拒否する。present-to-present置換時は同期TLB shootdownし、失敗時はPTEを
変えない。低位identity seed、512 KiB BAR、失敗rollback、再利用のkernel unitを
追加した。実機での成功はまだ未確認であり、次の冷間起動で一度だけprobeする。

次の冷間起動での単発probeでは、BAR5のhost MMIO mappingを通過し、Linux側で
VRAM 16304 MiBとGART 512 MiBの認識まで進んだ。従って低位identity seedとの
衝突は解消している。その後に独立した二つの障害が現れた。Linuxの
`get_vm_area_caller`は256 MiBの連続した`ioremap`仮想領域を要求したが、
従来のvmalloc窓自体が256 MiBでguard pageと既存割当が入らず失敗した。
またBAR0 `0xd0000000`の1 MiB `ioremap_wc`はhostへのcache=WC要求となり、
Pacha kernelが未対応cacheとして`INVALID`（adapterで`-EINVAL`）を返した。
`gmc_v12_0`は`-ENOMEM`で失敗し、後始末で二次的なUD2 faultを記録した。
gpudのDMA quarantineとsandbox停止は成功したが、GPU resetや再probeの安全性は
未証明である。Kobox2固有のvmalloc窓は低位の`0x50000000..0x80000000`に
768 MiBを確保するよう変更し、POSIX production bootで256 MiBの連続
`VM_IOREMAP`予約と解放を確認した。再ビルド済みpackageには反映したが、
実機へは未転送・未probeである。
WCは単にcache enumを受理しない。AMDのMTRR×PAT合成と、低16 GiB identity
aliasを含む全物理aliasの実効型が一致する証拠を必要とする。特にuser CR3の
identity PDはプロセスごとに複製され得るため、kernel PDだけの検査では不足する。
当時のGOP live consoleもBAR由来のframebufferをUCのMMIO FDとして保持し得た。
別のWC FDを追加するには、既存・将来の物理範囲単位cache-type leaseと、全aliasの
一致・解除時の同期まで設計する必要がある。これを満たさない限定WC受理案は
安全監査で不採用とし、その時点ではkernelのWC拒否を維持した。実機での再probeは行っていない。

次の実装方針はこの拒否を恒久的な制約とすることではない。Linux/amdgpuの
`ioremap_wc()`要求をそのままKobox2から渡し、既存のcapsule MMIO ABIのWC値を
PachaOSで実効WCとして成立させる。UCへの代用、AMDGPU固有の例外、Linux sourceの
変更はしない。物理範囲ごとに実効属性とMMIO FDの生存期間を管理し、GOP描画停止・
UC FD解放からWC取得への引き継ぎを行う。kernel/user CR3のidentity aliasと全CPUの
PAT/MTRR・TLB同期も同じ範囲の属性遷移として扱う。未対応という理由だけでWCを
一律拒否する現在の方針を緩める。BAR全体ではなく実際に重複する物理ページを判定し、
非重複部分のUC/WC併存まで妨げない。一方、実効属性が異なる生存aliasは作らない。
2026-09-26の実装で、通常kernelのPAT未使用slot 4へWCを各CPU起動時に設定し、
全online CPUのPAT/MTRR一致と物理範囲内のMTRR重複規則を検証してから実効WCを選ぶ。
低位identityの対象4 KiBページは一時的にnon-presentへ移し、kernelと各user CR3を
全CPUでshootdownする。CPU別の物理コピー窓に残った同一物理ページの旧PTEも
各CPUのIPIで退役させ、WC lease中はその窓から異なる属性で再マップさせない。
物理コピー窓のleaseは低位identityの有無に依存せず物理範囲で保持し、
高アドレスBARでも同じ重複排除・終了時同期を行う。
WBINVDで旧cache lineを排出してからWC user PTEを
公開する。最後のMMIO FD解放では逆順にWC PTEを消して同期し、元のidentity PTEを
復元する。重複WC leaseはページ単位で参照数を持ち、非重複UC windowは保持できる。
GOP live consoleのUC MMIO FDはgpudからの明示的なdisplay handoff要求で閉じる。
これは通常bootfsへの自動probe追加ではなく、既存capsule ABIを使う任意probe経路である。
kernelの294 unitとproduction buildは通過した。RX 9060 XTの実機再probeとGPU resetは未検証。
vmalloc変更後のproduction Kobox2 boot testでは256 MiB `VM_IOREMAP`の連続予約と
解放が通り、AMDGPU profileのcanonical/hosted build監査、PCIなしのfirmware
15件の検証・20 moduleのload/unload・後始末が通った。kernel 286 unitも通った。
これらはWCの安全性やRX 9060 XTのprobe成功を意味しない。

初回profileの検証は、別live imageのビルドとEFI内bootfsの照合、QEMUでの
`live services ready`→`optional GPU inspection armed; no probe until RAM command`、
PCIなしの20モジュールload/unload・firmware 15件の読取・失敗時cleanup、
既存gpudのbootstrap/lifecycle/process/launch単体試験まで。QEMUの標準VGAと
virtio GPUが同時に存在する場合は候補曖昧として拒否され、`-vga none -device ramfb`
で単一PCI GPUの待機経路を確認した。RX 9060 XTへのprobeは上記の失敗点まで実施。
driver bind、render node、ring/IRQ、正常停止は未達。

2026-09-26の次の冷間起動では、MSIの単一message経路を使い、Kobox2の
`vmalloc`散在RAMページを既存のpage-view VMOでまとめて公開したpackageを、
起動USBを更新せずSSHでRAMへ転送・SHA照合した。明示的な一回のprobeで
`host route allocated mode=1`、SMU/DC/DMUB初期化、gfx/compute/SDMA/VCN/JPEGの
ring登録、`Initialized amdgpu 3.64.0 ... minor 0`、
`gpud: PCI DRM probe ready status=0`まで到達した。probe後もSSHは維持された。
`stop\n`ではLinuxの`finishing device`と`ttm finalized`、MSI routeの解除
（`routes_empty=1 parent_maps=0`）、`gpud: PCI DRM probe stopped status=0`を確認。
gpudの停止経路はDMAを無効化してからPCI resourceを解放する。

これはLinux内部のDRM登録と安全停止の実機成功であり、PachaOS側の
`/dev/dri/renderD*`公開やrender clientからのopen成功ではない。現行の任意
`PCI_DRM_INSPECT`経路はLinux READY後に戻り、通常のgpud DRM query serviceを
作らないため、実機shellの`/dev/dri`は存在しない。またring登録ログは
ジョブ投入・IRQ完了の証明ではない。次の段階は機種固有条件をseed0rootへ
置かず、gpudの共通DRM経路にrender nodeの発見・openと最小の非表示
ジョブ/IRQ確認を接続すること。GOP画面はGPUへの引き継ぎ後に自動復帰しない。

同日の次の冷間起動で、既存のKobox2 DRM file経路に段階別のRAM診断を追加し、
修正版packageをSSHでRAMへ転送・照合して一度だけ実機probeした。許可された
PCI機能の子からprimary `226:0`、render `226:128`を発見し、render openは
`status=0`。通常のLinux DRM ioctl経路で`DRM_IOCTL_VERSION`は
`driver=amdgpu version=3.64.0 status=0`、`DRM_IOCTL_GET_CAP(PRIME)`は
`value=0x3 status=0`を返した。明示的な停止ではrender close `status=0`、
MSI route解除、`gpud: PCI DRM probe stopped status=0`を確認した。
Linuxのrender fileをKobox2内部で開いて基本問い合わせを通した段階であり、
PachaOSの`/dev/dri`公開、GPUジョブ投入、IRQ完了、描画はまだ実施していない。

## 共通 DRM 公開経路（2026-09-27）

`gpud` の DRM endpoint、ファイル所有・セッション管理、GPU command queue は
virtio-gpu と AMDGPU で共有する。プロファイルの選択は `gpud` と Kobox2 の
起動契約で行い、`seed0root` は PCI ID や GPU 固有 ioctl を知らない。
AMDGPU の任意 profile では render node (`renderD128`) だけを公開し、現時点で
転送を許す ioctl は DRM 共通の VERSION、GET_CAP、SET_CLIENT_CAP、GEM_CLOSE、
syncobj 基本操作に限る。virtio-gpu 固有コマンドと primary/KMS node は拒否する。
AMDGPU 固有 ioctl は、型付きの変換・検証経路を追加するまで未対応であり、
この段階で Mesa による描画や計算が可能になったとは見なさない。

任意 profile が endpoint を公開した後も、実機 GPU には手動 `probe` まで触れない。
プローブ前に render node を使ったクライアントには `ENODEV` を返し、IPC の
待ちっぱなしを避ける。プローブ中は既存の温度・時間上限、stop、DMA quarantine、
停止後の速やかな電源断という運用を維持する。共通 DRM コマンドのコード検査、
Kobox2 の no-device ビルド、および live image の作成は実機 render node の
動作確認とは別である。

## AMDGPU render job（2026-09-27）

LPR → `gpud` → Kobox2 の共通 DRM file/session 経路に、AMDGPU_INFO、GTT GEM
create/mmap、GPU VA、context、CS、WAIT_CS を型付きで接続した。Linux UAPI の
ポインタは境界ごとに長さ・方向・個数を検証してからコピーし、Linux amdgpu 本体は
変更していない。GTT の CPU page-view は Kobox2 の `drm_mapping` module が
Linux TTM の公開 API で populate・pin し、mapping lease の解放時に unpin する。
この module は AMDGPU package の closure に含め、DRM/TTM の後、amdgpu root の
前にロードする。package 生成時に実際の Kobox2 closure seal も検証するため、
孤立した追加 module は配布前に拒否される。virtio-gpu 用の同 module も別構成で
ビルド確認済み。

RX 9060 XT / Ryzen 9 5900X 実機で、SSH と RAM ログ、70°C/120秒のguest上限、
150秒のhost上限、停止直後の電源断を使い、一回の手動probeで確認した。
PachaOSの利用者プロセスが `/dev/dri/renderD128` を開き、VERSION・GET_CAP・
AMDGPU_INFO、GTT BO二個のmmap、VA map、SDMA CSを通した。fence待機の後、
出力BOの16 dwordがすべて `0x6a25d4c3` で一致した（sequence 1）。実測温度は
47〜49°C。render close、TTM finalized、MSI detach（routes_empty=1）、
`PCI DRM probe stopped status=0`を確認し、実機の電源断も確認した。
PCIなしのfirmware 15件・21 moduleのload/unloadとcleanupも通過した。

これは非表示の小さな SDMA job が実際に完了した証拠であり、Mesaによる描画、
KMS表示、複数ジョブや長時間運転、GPU reset/reprobeの安全性はまだ示さない。
次段階で Mesa と実画面を扱うまでは、通常bootfsの自動GPU起動にはしない。

## AMDGPU 用 Mesa runtime package（2026-09-27）

`python3 pack/scripts/build_amdgpu_mesa_package.py` は既存の固定版
Alpine Mesa 25.1.9 runtime と共有 LLVM runtime から、AMDGPU の
GBM/EGL/GLES2 描画に必要なファイルだけを
`.artifacts/kobox2-amdgpu-mesa-package/rootfs` に配置する。
`--verify` は成果物の全ファイルと SHA-256 を独立に照合する。
通常の live bootfs、通常 rootfs、AMDGPU の firmware/module package は変更しない。

入口は EGL、GLES2、GBM、libdrm、libdrm_amdgpu、動的ロードされる
`radeonsi_dri.so` と `dri_gbm.so`。ELF の `DT_NEEDED` を再帰的にたどり、
Mesa/LLVM の共有ライブラリを追加する。`libc.musl-x86_64.so.1` だけは
live bootfs に既にある外部依存として検証する。`amdgpu.ids`、
Mesa のデフォルト drirc、既存ビルドの出自記録も含める。
rootfs のリンクマーカーはリンク先の存在と循環・rootfs 外への脱出を検査する。
Mesa のソースや既存バイナリには変更を加えない。

現行成果物は 70 ファイル、236,547,420 bytes（`du` 約226 MiB）。
内訳の大半は `libLLVM.so.20.1` の178,432,672 bytesと
`libgallium-25.1.9.so` の49,944,120 bytesである。従来の Mesa root の
約159 MiB という値には別rootの LLVM runtime が含まれていなかった。
実機 GPU の EGL 描画はまだ未検証。次の作業で Mesa の実際の ioctl 要求を
観測し、LPR・gpud・Kobox2 を必要な範囲で接続する。

## Mesa 専用 live bootfs と QEMU 起動確認（2026-09-27）

通常の live bootfs は変更せず、AMDGPU 専用 overlay の
`/srv/amdgpu-mesa-runtime.tar.gz` に上記70ファイルの検証済み closureを格納する。
アーカイブは gzip timestamp を固定して再ビルド時の SHA-256 を安定させ、
overlay でもハッシュを固定する。Linux AMDGPU driver/firmware package は引き続き
手動 RAM 転送であり、GPU probe は自動実行しない。

起動時には Limine の BOOTFS module を CR3 切替前に散在する物理ページへ退避し、
固定32 MiBの kernel scratch にはページ番号の一覧と init ELF だけを置く。
init VMO へのコピー後、退避ページを解放する。BOOTFS の独立上限は128 MiB、
展開後 RAM root の上限は192 MiB。tmpfs は512 MiBまで書き込み時にページを
確保し、32-bitページIDと順次読み書き用のページ索引を使う。
専用の Limine image は入力サイズに応じて192 MiBへ拡大する。

最終イメージの QEMU/KVM・OVMF・4 GiB RAM 試験では、ash と SSH が起動し、
`filed` は82ファイル・148,205,913 bytes の RAM root を作成した。
ゲスト内の Mesa アーカイブ SHA-256 は
`d911b9bc22daf4ac44c373bbbaa0c72b36bd9bed2cf5bd04716d7c9fb3d4e457` と一致。
RAM展開後の専用チェックは `MESA_RUNTIME_READY egl=loaded radeonsi=loaded
gpu=not-probed` で成功した。QEMUに AMDGPU render node は無いため、通常描画
テストは `render-open` で失敗するのが期待値であり、Mesaの実描画成功とは扱わない。

### 実機起動の成果物不一致と IOMMU 診断

実機の最初の黒画面では、USB更新に使った出力ディレクトリの `KERNEL.ELF`
が古く、QEMUで起動したimage内部のカーネルと異なっていた。
Limine image builder は入力を同じ出力ディレクトリの `KERNEL.ELF`、
`INITAPP.ELF`、`BOOTFS.IMG`、任意の `DIAGBOOT.ELF` に同期し、そのファイルを
imageへ収めるよう変更した。古いloose kernelを更新する回帰試験を追加し、
生成imageから抽出したkernelとloose kernelのSHA-256一致も確認した。

現在のコードから作った診断kernelでは、実機はAMD IOMMUの
`unit preparation failed index=0` で停止した。UC証明失敗・既に有効のログは
報告されていない。DTE、command/event/completionページの確保・ゼロ初期化の
失敗にstage、error、要求ページ数、物理アドレス、4GiB未満の空き総量と
最大連続領域を追加した。割当方針は変更していない。

追加診断版のQEMU/KVM・OVMF・AMD IOMMU・32GiB/24CPU試験は、標準配置と
`max-ram-below-4g=384M` の両方でIOMMU初期化、RAM root、ashまで成功した。
実機のIOMMU失敗の根本原因は未確定であり、このQEMU成功で解決とはしない。

その後の実機ログで `stage=device-table error=OutOfFreePages`、要求512ページに
対して `below4G free_pages=256 largest_run=256` を確認し、低位RAM不足と確定した。
bootfs退避は汎用の範囲指定allocatorで `[4GiB, identity_limit)` を優先し、
その範囲が空の時に低位へfallbackする。退避前後の低位空きページ数も記録する。
107MiB退避後も低位の連続領域が残り、512ページのDTEを確保できること、
高位RAMなしのfallback、上下境界・失敗時不変性を単体試験した。
IOMMUのDMA隔離条件や公開ABIは変更していない。修正後の実機再試験は必要。

修正後の32GiB/24CPU・AMD IOMMU・低位384MiB QEMUでは、
`staged pages=27347 below4G=0 free_low_before=16412 free_low_after=16412`
を記録し、IOMMUとashの起動に成功した。追加の2GiB構成では修正前後とも
`bootfs staging page unavailable` で停止したため、大容量Mesa imageの低RAM起動は
未対応として残る（今回の高位優先による回帰ではない）。

## Mesa discovery の接続修正（2026-09-27）

高位RAM退避版では実機のSSH到達を確認。最初のMesa実機試験はAMDGPUの
probe、render open、VERSIONまでは成功したが、デバイス情報取得と
`amdgpu_get_auth` に失敗した。温度は48→45°C、stop status=0を記録し、
ユーザーが完全な電源断を確認した。描画成功とは扱わない。

固定版libdrm 2.4.124の `drmNodeIsDRM` はmajor番号だけでなく
`/sys/dev/char/<major>:<minor>/device/drm` をstatする。この公開情報がなく、
render nodeを識別できず、本来不要なGET_CLIENTに進んでいた。
gpudの `discovery.c` / `discovery_filed.c` で、所有capabilityから読み取った
PCI ID、subsystem ID、revision、BDFをLinux互換のRAM上の発見用ツリーへ公開する。
これはドライバ選択ではなく、同じデバイスの読み取り情報の公開である。
AMDGPU inspectionではrenderのみ、既存virtioサービスではprimaryも列挙する。
公開はsandbox起動成功後。失敗時と停止時は自分が作成した項目を逆順で撤去し、
既存の個別デバイス情報を上書きしない。kernel/seed0root/公開ABIは変更しない。

GBMのMesa EGL backendはpbuffer configを提供しないため、試験は
surfaceless EGL context + RGBA FBOの64x64描画へ変更した。
node type、PCI discovery、EGL、renderer、FBO、shader、readbackの段階を記録する。
実AMDGPU rendererの必須条件とソフトウェアfallback拒否は維持する。

検証:
- `tests/run-gpud-discovery-unit.sh`: capability情報、全公開RPC失敗注入、逆順撤去。
- `tests/run-gpud-discovery-libdrm.sh`: 配布版の無改変libdrmに生成ツリーを渡し、
  render判定、BDF/PCI/subsystem/revision取得が成功。ホストGPUは使わず、
  デバイスノードのstatのみシミュレーションする。
- LPR/gpud connection回帰試験成功、gpudと描画クライアントのビルド成功。
- 更新イメージの32GiB/24CPU・低位384MiB・AMD IOMMU QEMUでash/USB HID成功。
  QEMUはGPU描画の合格証拠ではなく、修正後の実機Mesa描画は次回試験が必要。

描画クライアントの再ビルドは `pack/scripts/build_amdgpu_mesa_render.sh`。
OSSソース・バイナリには変更を加えない。

### 実機 discovery 成功と INFO サイズ互換性

更新版の実機で `MESA_RENDER_NODE type=2 expected=2`、
`MESA_RENDER_DISCOVERY status=0`、実PCI ID `1002:7590` を確認した。
次にlibdrmの `amdgpu_query_gpu_info_init` が失敗し、llvmpipeへのfallbackを
試験が明示的に拒否した。温度44〜46°C、stop status=0、物理電源断を確認。
ログ: `.artifacts/amdgpu-inspections/run-20260927T094848Z-Pl7JMJ/`。

配布版libdrm 2.4.124のDEV_INFO/HW_IP_INFOは440/32 bytes、新しいLinux側の
native layoutは448/40 bytes。LPRの完全一致検査でDEV_INFOをLinuxへ送る前に
EINVALとしていた。LPRは要求容量を別に保持し、既知の型をcanonical recordへ
変換してから `min(利用者容量, nativeサイズ)` だけ返す。要求を越えてコピーせず、
大きな利用者バッファの末尾も上書きしない。scalar INFOも同じ規則にそろえる。
LPRの事前検証・補助領域確保・RPC失敗にはquery、元のbytes、phase、errnoを
stderrへ記録し、既存のアプリRAMログに残す。kernelログへの依存は追加しない。

その先でlibdrmが必ず行うREAD_MMR_REGを、既存のINFO requestの4 selectorと
U32配列spanで接続した。1〜128件・長さ・offsetの加算範囲を検証し、
Linux側の専用バッファへ受け取る。レジスタ許可判定はupstream amdgpuに残す。
公開wire ABI/番号の変更はない。Kobox内の関数境界は4 selectorを渡す形へ更新し、
core/sandboxを一緒に再ビルドする。

回帰試験は配布版libdrmヘッダの440/32-byte要求、prefix/余剰容量/境界、
MMR件数とoverflow、gpud→Kobox2→completionの32件応答、拒否ログを対象とする。
これらの静的・ホスト試験は実機radeonsi描画成功を意味しない。

修正後のLPR/gpud/core/sandboxをビルドし、PCIなしのfirmware試験と
21モジュールのload/unload（cleanup=0）が成功した。32GiB/24CPU・
低位384MiB・AMD IOMMUのQEMUでもashとUSB HIDまで起動確認済み。
Disk 3 General UDiskのBOOTFS.IMGだけを更新し、読み戻しSHA-256は
`374f0cb76926c916f5dabf3f10409678b832aeaa7ca5c03ecefea9dabb31889c`。
旧版は`BOOTFS.PREINFO`に保存、KERNEL.ELFは変更していない。
次の実機試験では再ビルド済みの任意GPUパッケージをSSHでRAMへ転送する。
