# Mesa render 接続の静的監査

対象は配布版 Mesa 25.1.9 / libdrm 2.4.124、通常の radeonsi、
`gbm_create_device → surfaceless EGL → GLES shader → FBO → glReadPixels`。
ソフトウェア fallback は成功としない。上流ソースは読み取りのみ。

## ソースから確認した必須経路

| 段階 | 上流の入口 | 接続と検証 |
|---|---|---|
| DRM discovery | libdrm device_initialize / Mesa amdgpu_winsys_create | VERSION、render node、PCI情報、OFD/session寿命。既存実機で確認済み |
| GPU情報 | ac_gpu_info.c: ac_query_gpu_info | DEV_INFO、HW_IP_INFO/COUNT、FW_VERSION、MEMORY、VIDEO_CAPS、MAX_IBS |
| libdrm初期化 | amdgpu_gpu_info.c | DEV_INFOの旧版prefix、READ_MMR_REG、VRAM_GTT/heap使用量 |
| 割り当て | amdgpu_bo.c: amdgpu_bo_create | GEM_CREATE、GEM_MMAP、GEM_VA、GEM_CLOSE。mmapの部分解除・leaseは既存回帰試験 |
| GFX12 texture | si_texture.c: si_texture_create_object | **共有やscanoutの有無にかかわらずGEM_METADATA SETが必要**。nativeのdata offset 28とwire offset 32を変換 |
| context | amdgpu_cs.cpp: context作成/リセット検査 | CTX ALLOC/FREEにQUERY_STATE/QUERY_STATE2を追加。priorityはLinuxへ渡す |
| submit | amdgpu_cs.cpp: amdgpu_cs_submit_ib | BO_HANDLES、SYNCOBJ_IN/OUT、USER_FENCE、preamble/main複数IB、GFX_SHADOW |
| 同期・再利用 | amdgpu_cs.cpp / amdgpu_bo.c | binary syncobj CREATE/DESTROY/WAIT、CS fence、GEM_WAIT_IDLE（0=poll、絶対期限、無期限） |

INFOは独立した共通shape定義で、queryごとのrecord/count/bytes/selectorを検証する。
FW_VERSIONはtype/instance/indexの3つを渡す。MAX_IBSは単一u32ではなく
IP別の10要素配列であり、古い利用者には要求容量まで返す。
Linux側の固定ヘッダが変われば静的assertで検出する。

## CS の所有権と制限

LPRはユーザーのchunk pointer配列を読んで、既存schemaの型付きspanに変換する。
IBだけはnative配置とwire配置が異なるので並べ直す。
BO_HANDLES内のoperation/list_handleはLinuxが読まないため捨てる。
Mesaがそこへ`~0`を入れるのを拒否してはいけない。
BO_INFO_PTRは内容のコピー後に捨て、sandboxへ送らない。

gpudとsandboxは、型・件数・長さ・累積offsetをそれぞれ検査する。
64 chunk / 1 MiBを上限とし、schema既定の型をすべて扱う。
Linux ioctlへ渡すchunk配列・IB・BO list・pointer配列・payloadはLinux所有の
一時領域で構成し、成功・失敗とも解放する。GPU VA、handle、syncobjの妥当性と
ASIC固有の検証はupstream driverに残す。

一連の型付きspanが1ページを超えるため、ホスト内のrequest stagingを16 KiBへ
拡張した。reply/outputとの分離と両側のarena/VMOサイズも同時に更新する。
64 chunkのencode/decode/prepareが旧4 KiBを超えても収まることを試験する。
カーネルABIやioctl番号を新設しない。LPR↔gpudのprivate CS表現とnative transport
は旧版と混在させず、一緒に再ビルドする。共通wire recordの番号は変更しない。
GEM_WAIT_IDLEのdeadline=0は正当なpollとして受け入れる。他のwaitの規則は変更しない。

## 今回の描画試験に含まれない経路

- `AMD_USERQ`は配布版の既定値がfalse。user queue作成・doorbell・VM同期拡張は対象外。
- dma-buf画像の外部import/export、GEM_OPのcreate-info取得、display/KMS、
  外部timeline/sync-fileの交換は今回のsurfaceless FBO試験とは別の合格条件。
  完全なMesa/DRM実装であるとは主張しない。
- `os_same_file_description`の警告は別課題。kcmp/OFD比較の不足を警告の抑制や
  無条件成功で隠さない。既存のdup/close/inflightテストは継続する。

## 検証

静的回帰試験の入口: `bash tests/run-amdgpu-mesa-static.sh`。

- `run-lpr-amdgpu-info-unit.sh`: 配布libdrmの440/32-byte構造体と要求prefix、
  firmware/memory/video/MAX_IBSの容量、MMRとreserved selectorの拒否。
- `run-lpr-drm-connection-unit.sh`: Mesa形状のBO/sync/fence/複数IB、
  不正chunkとIB、途中失敗のaux解放、既存接続・fork・再利用。
- `run-gpud-drm-translate-unit.sh`: INFO、metadata配置と256-byte上限、
  context query、idle poll/無期限、CSのcanonical encoding。
- `run-gpud-gpu-query-unit.sh`: 実encoder→private plan→mock Linux API→
  completion→gpud reply、INFO出力guard、複数IBと64 chunk上限。
- `amdgpu_uapi_layout_unit.c`: 実Linuxヘッダのioctl番号・nativeサイズ・offset。
- 既存のmapping lease、256 mappings、OFD dup/close、非同期fenceの回帰試験。

ホスト試験やPCIなしロード成功はGPU描画成功ではない。実機では一回のprobeに
温度・時間上限、実AMDGPU renderer、画素readback、正常stopと直後の電源断を要求する。

今回の結果: 静的回帰gate、LPR/gpud/sandboxのビルド、固定Linux hosted linkが成功。
PCIなしfirmware検査と21モジュールのload/unloadは`cleanup=0`。
32 GiB / 24 CPU / AMD IOMMU / 低位RAM 384 MiBのQEMUでashとUSB HIDまで起動。
実機プローブとUSB更新はこの静的監査では実施していない。

新bootfs SHA-256:
`5c867112619e8af522f7e520c3475666ab26a5fe7e35cfc3b41eb05750df2773`。
検証イメージ更新時、mformatにpartition長を渡さない既存処理がbackup GPTまで
FATとして利用する問題も発見した。末尾33 sectorを除くpartition長を明示し、
生成側の単体試験・GPT検査・イメージ内bootfsの読み戻しハッシュ一致を確認した。

## 2026-09-27 実機追試と GEM_VA direction 互換

更新USBのgpud/LPRハッシュをSSHで照合し、一回の監視付きprobeを実行。
`run-20260927T105019Z-0HMVaq`にログを保存した。GBM backend=drm、
EGL 1.5まで進み、context作成でIB buffer確保が失敗した。描画合格ではない。
温度サンプルは42–44°C、stop status=0の後に電源断を要求し、利用者が完全停止を確認。

失敗要求は`0xc0406448`。LinuxのGEM_VAマクロは`0x40406448`だが、
固定libdrmの`amdgpu_bo_va_op_raw`は`drmCommandWriteRead`で前者を生成する。
Linux DRMはordinalでdriver定義を選び、入力・出力方向を交差させるため受理する。
gpudにもこの明示的な別名を追加し、同じGEM_VA型付きcommandへ変換する。
全ioctlのdirection/sizeを無視する変更ではなく、64-byte長・reserved・span検証を維持。
両形式のcommand一致、不正長/reserved/read-only形式の拒否を回帰試験に追加した。
今回対象の他のlibdrm呼び出しも方向を照合し、同じ差異は見つからなかった。

## VRAM CPU mapping: implementation work remaining (no KMS)

実機追試 `run-20260927T105616Z-r2x1yS` ではGEM_VAの失敗が消え、
`border_color_buffer` のGEM_MMAP (`0xc0086441`) がdriver-replyで
`-EOPNOTSUPP`になった。EGL contextは未作成、描画合格ではない。
温度43–46°C、stop status=0、電源断後の完全停止を利用者が確認済み。

固定Mesaの`si_buffer.c`はPIPE_USAGE_DEFAULTをVRAM/GTT_WCとして確保する。
一方、Koboxの`gem/drm_mapping.c`はTTM_PL_TTかつttm_cachedしか公開しない。
さらにnative adapterのattachmentはsandbox RAMのpage indexだけを受け付ける。
この二つの制限を単に削除すると、BARのPFNをRAM indexと誤認する。

根本対応の責務分担（カーネル範囲は専用レビューで承認済み）:

- Linux側: upstreamのCPUアクセス検査、TTMのreservation・移動完了待ち、
  CPU-visible placementとページ解決を利用する。VRAMのPFNはdriverの
  `io_mem_pfn`に従い、`resource->start + index`で連続と仮定しない。
  BOは公開leaseの間pinし、失敗時もreservation/referenceを戻す。
- Native adapter: backingがsandbox RAMかBARかを明示的に区別する。
  物理アドレスを利用者に渡さず、保有PCI capabilityのBAR範囲に照合して
  必要ページだけのmemory capabilityを作る。型・長さ・属性を各境界で検証。
- Kernel: 汎用device-memory VMOを通じて、BAR範囲・キャッシュ属性・leaseを
  保持した転送とshared/nonexec mmapを提供する。AMDGPU/TTM/Mesaは知らない。
  現MMIO FDはowner VAを持ちtransfer=falseなので、その制限を外すだけでは不可。
  faultとuser-copyにも属性を伝え、WCにWB aliasを作らない。未マップのVMOも
  物理属性leaseの所有者に数え、最終解放でBARページをPMMへ返却しない。
- gpud: 既存OFDとmapping leaseを継続利用する。全native mappingのrevokeを
  完了してからLinux側をunpinする。失敗を成功扱いせずclose/stopに伝える。

WC RAMは同じ属性名でもBARとは別で、sandboxのanonymous親のWB aliasがある。
PAGE_VIEWをWCにするだけでは不正。全aliasの属性整合とRAM再利用前のWB復元を
含めて扱う。VRAMを強制的にWB RAMへ移す、CPU側shadow copyで描画を装う、
Mesaの配置要求を書き換える回避策は採らない。

必要な回帰試験: BAR境界/overflow/権限減衰、非連続ページ、cache競合、
FD転送後の所有者close、partial unmap/fork、revoke後のfault、途中失敗rollback、
全lease解放後の属性復元、RAMページ会計不変。既存virtio mappingも再確認する。
最終合格は実AMDGPU rendererでshader描画→readback一致→正常stop/電源断。
この節は実装方針と未完了事項であり、device-memory対応の完了を示さない。
display/KMSはこの目標に含めない。

### Device-memory backing implementation checkpoint

KernelStateにdevice-memory backingを追加した。BAR相対の非連続page offsetを
範囲・整列・overflow検査して保存し、作成後のページ差し替えを拒否する。
解放はbacking metadataのみで、BARページをfree listへ返さない。anonymous親と
device metadataをunionで保持し、VMO slotはDebugでも64 bytes以内に収める
（以前56 bytes、32768 slots全体で最大256 KiB増）。外部ABIはまだ変更していない。
新しいsyscallから公開しておらず、cache-aware fault未接続のためVMA作成も拒否。
この段階のbackingだけを実機へ配布してもMesaは改善しないため、USB更新はしない。

`zig build test`と`zig build kernel`は成功。追加試験は非連続pageの解決、
範囲外/overflow/未整列/無効deviceの拒否、immutable backing、RAM page-view作成の
拒否、未完成のWB fault経路への流入拒否、最終releaseでfree page数不変を確認。

次に必要なcache lease実装は最終ref dropで全VM lockを取得してはいけない。
fault側のaddress-space→shared-VM lockと逆順になりABBAになるため、最終dropで
fallibleな確保をせずpending retirementに移し、既定ロック順の外側でdrainする。
pending中も物理属性競合とcopy-window制限を保持し、PTE解除/shootdown、flush、
identity restoreが終わるまでleaseを再利用しない。通常returnだけでなく
exec/exit/fatal teardownにも接続する。既存mmio_regionのuser_va=0ではdestructorと
競合検査の両方がskipするため、そのままleaseとして流用しない。

### 2026-09-27 Device-memory publication and deployment candidate

上記checkpoint後、PCI BARからimmutableなdevice-memory VMOを発行する経路、
cache-aware fault/user-copy、mprotect時のPAT保持、deferred retirementを接続した。
既存DERIVE_MMIOの`MMIO_EXPORT_VMO`モードは、ユーザーVAの代わりにBAR相対の
page-offset配列を受け取り、転送可能なVMOを返す。新しいsyscall番号は増やさない。
このABI追加の理由は、既存MMIO FDがowner VAに結びつき転送不能であり、
GEM利用者へ必要ページと寿命を安全に委譲する仕組みがなかったため。
カーネルはGPU/TTMを知らず、PCI範囲・shared/nonexec・属性・参照寿命だけを扱う。
BARページをDMA用RAMとしてpinすることも拒否する。

Linux providerはTTMの移動完了待ち・pinとdriverのio_mem_reserve/io_mem_pfnを使い、
resident VRAMを解決する。native側で全PFNが同一の保有BAR内に収まることを検査。
RAM indexとdevice PFNをprivate metadataで区別し、利用者にはPFNを渡さない。
WB/WC/UC completionを明示化し、範囲外のpolicyは拒否。PRIMEはまだWB RAM専用で、
device backingを誤解釈しないよう公開前に拒否する。

未完了: invisible VRAMをCPU-visible placementへ移す経路、sandbox WB aliasを伴う
WC RAMの属性遷移。これらをRAM placement強制やキャッシュ属性の偽装で回避しない。
そのため今回の接続だけで全Mesa allocationが通るとはまだ主張しない。
失敗ログはprovider側のmem_type/cache/bytesとnative publication stageを残す。

ローカル試験: kernel 300/300、Mesa static compatibility gate、PCI exportの
非連続page・同一BAR・overflow・権限・cacheの拒否試験（GCC/Clang）に成功。
device VMOの転送・fork・mprotect・revoke・pending中のcache競合・最終解放で
RAM会計不変、copy windowのPATと退役も確認。Linuxモジュール21個のPCIなし
load/unloadはcleanup=0。これらは実機描画成功の証明ではない。

32 GiB/24 CPU/AMD IOMMU/低位RAM384 MiBのQEMUで更新kernel/bootfsを起動し、
USB keyboardから`busybox echo vmo_boot_ok`を実行、出力とprompt復帰を確認した。
QEMUのSSHはbanner timeoutで未確認（NIC ready/carrier=1まではログあり）。
試験後のQEMUはホスト実行管理からSIGTERMで終了し、guest crashではない。
live-image全依存再buildはnetwork profileのclang固定版検査で止まったため、
network成果物は既存の実機検証済み版を維持し、live-bootfs --no-buildと
明示的なLimine artifact更新で構成した。imageからkernel/bootfsを読み戻し照合済み。

今回の配布候補SHA-256:

- KERNEL.ELF: `cf555f5933fc4ebaf72ecc2ef2e084d5b2a1ee398fd86925023d7c2b035f8b7c`
- DIAGBOOT.ELF: `e84cfc96f031cb0d05b08d6355fae3998253b0cbf7c589d2abe456083f737f5d`
- BOOTFS.IMG: `33960ebcfc4429ca99719f18ba00b03f3037c440b4720cac2e2b08f1e2accb93`
- AMDGPU core: `1c3a03dfd70c6c237d9f25364fd294fb2963743f421bc302e8ad58c0e3982fe6`
- sandbox: `07f7eada0145bbe5c96988b911c8c434fcd939ac2a8ac58c5ed9baa15d079d44`
- mapping module: `92438d697c2b03c8df1453d3580658231ab062a032984a0434855cc047358ff1`

実機では、このkernel/bootfsで再起動後にSSHへ接続し、同一packageをRAM転送して
一回の監視付きMesa試験を行う。stop後は即poweroff、確認質問は毎回繰り返さない。

### 2026-09-27: device VMO 実機試験と USB 内パッケージ

上記版の実機試験ログは `.artifacts/amdgpu-inspections/run-20260927T114104Z-drte3K/`。
複数の GEM_MMAP の後、GEM_CREATE（0xc0206440）で transport=-32、
probe stopped=-32 となった。温度は44→43→42℃、停止検出後に電源断を要求。
これは描画成功でも正常停止の証明でもない。GL_RENDERER と描画読み戻しは未達。

起動完了後は例外・fatal reporter が管理チャネルへ通知しない実装だったため、
既存の SEND-only ログ capability に直接記録するよう修正した。カーネル変更なし。
一次例外はスタック走査より先に記録する。ホスト試験側も予期しない停止時に
mesa-render.log を回収してから電源断する。fault ログの値・無効FD時の動作は
GCC/Clang単体試験で確認したが、次の実機試験で停止原因を確定する必要がある。

専用 bootfs に Mesa と driver archive / digest を同梱した。通常 live は変更しない。
起動後はハッシュ確認した内蔵 archive を RAM 展開し、ホストの現行版と各ファイルを
比較する。同じ版は転送なし、再build後は変更ファイルだけ送る。RAM policy は最後に
照合して公開し、手動 probe まで GPU に触らない。archive 決定性・収録範囲・
symlink/非通常ファイル拒否、および変更ファイルのみ転送する模擬SSH試験に成功。

USB Disk 3 General UDisk の BOOTFS.IMG のみ更新・flush・読み戻し照合済み。
旧版は BOOTFS.PREPKG、kernel/diag は上記版を維持。

- bootfs: 122013103 bytes / `3bb7a69c724b4a4cfe86287d0ed6ee8b87487d164d7eb3cec9ff626ae818515d`
- driver archive: `99f6faf02c4408783261bcfd7fdaf203960f10c60a1d2fb561231d202aaff596`
- sandbox: `3a7003661c1a6af7f876a2d2e2308e1d50c5c208443ebf0de298a23fa8964484`

新しい bootfs の実機描画は未検証。KMS は引き続き対象外。

### 2026-09-27: 同梱版実機試験、GTT キャッシュ属性の未接続を特定

`.artifacts/amdgpu-inspections/run-20260927T115620Z-wWHjGF/` で、内蔵 driver
archive の展開と全バイナリの再利用を確認（driver 全量アップロードなし）。
mesa-render-guest.log は次まで成功している:

- renderD128 は amdgpu 3.64.0、PCI 1002:7590
- GBM backend=drm、EGL 1.5 / Mesa Project
- GL_RENDERER=AMD Radeon Graphics (radeonsi, gfx1200, LLVM 20.1.8, DRM 3.64, 6.12.0)
- OpenGL ES 3.2 Mesa 25.1.9

その後、size=2097152 alignment=2097152 domains=2 flags=4 の GEM 作成で停止。
新しい RAM ログが vector=13 error=0 core_relative=1 ip=0x69cc1 を捕捉した。
配布した core の link base=0x10000000 を加えた 0x10069cc1 を
addr2line/objdump で照合すると、__cpa_flush_all の WBINVD 命令そのものだった。
Linux CPA が set_pages_array_wc の UC_MINUS→WC 遷移で cache flush を要求する
一方、現行 early_boot.c の protect_direct_range は RWX のみホストへ渡す。
特権命令を無視したり CLFLUSH capability の設定だけで逃げても実 WC の接続には
ならない。GEM_MMAP の VRAM BAR export と GTT RAM cache transition は別経路。

温度45→43℃、失敗捕捉後は停止検出・電源断要求済み。描画/readback は未達。
次の修正は、canonical RAM backing の page 単位 cache 状態と全 alias に対する
native transition、および Kobox の CPA/host memory 境界の接続。上流コードは
変更しない。kernel 作業の必要性は AGENTS 指定の subagent review 済み。

承認条件:

- PCI BAR capability を RAM authority として流用しない。owned/populated VMO
  の範囲から解決し、借用 view の権限昇格を許さない。
- PAT/MTRR、範囲、全 alias、必要 metadata/PTE allocation を事前検査。
- VM lock だけでは実行中 CPU を止められない。旧 user/supervisor/copy-window
  alias をすべて撤去・全 CPU TLB acknowledgement 後に cache drain、新属性公開。
- fault/fork/mprotect/page-view/user-copy は同一の物理 cache 状態を参照。
- PMM 返却・再ゼロ化前に WB 復元。異常終了と revoke も対象。内側ロックから
  全 VM ロックを取らず、必要なら最終解放を外側へ延期。
- DMA の実行中同期を勝手に推定しない。移行できない競合は明示拒否。
- WB→WC→WB、UC、別 process/view、失敗 rollback、cross-CPU ack、crash時の
  復元試験が必要。既存 BAR export 試験だけで RAM の正しさを主張しない。

実装中の kernel foundation:

- `memory/ram_cache_policy.zig`: canonical owner token を持つ非WB物理範囲表。
  事前計算は別 scratch にだけ書き、split/merge/WB復元/owner競合を扱う。
  WBの隙間の所有権は、この表とは別に VMO から証明する必要がある。
- `memory/ram_cache_transition.zig`: preflight後、全alias撤去→全CPU TLB ack→
  cache drain→policy commit→公開→再TLB ack の順序を定義。
- 7単体試験（544回の範囲/属性変更とページ単位参照との照合を含む）に成功。
  通常 `zig build test` に追加し、その全体実行も exit=0。

これらはまだ syscall/PTE/fault/PMM 経路に接続していない。配布kernelは未変更。
copy window は IRQ禁止区間内で immutable snapshot を一度 acquire-load し、
pending範囲ならスピンせずguardを離れる設計で接続する。writerはpendingを公開して
からalias撤去、全CPU ack後に旧snapshotを回収する。cache lockや奇数seqcountで
IRQ禁止readerを待たせると、writerが待つIPIを処理できずdeadlockになるため不可。
最終alias公開と再TLB ackまではpendingを維持し、その後final policyを公開する。

次の接続を追加し、`zig build test` の再実行も exit=0:

- user_copy の物理コピー窓が acquire-load した immutable snapshot の PAT を使う。
  pending 範囲は無待機で拒否し、異なる明示UC/device属性での再aliasも拒否する。
  非WBのコピー窓ではguardを離れる前にMFENCEを行う。
- native VMA fault mapper は AS lock 内で同じ物理policyを調べ、WC/UC/UC_MINUS
  を実PTEへ反映する。pendingやcached executable mappingは拒否。
  mock page tableでPAT符号・pending拒否・新規PTE非公開を試験した。
- nativeRamCachePages は canonical anonymous VMO、世代、所有元token、範囲、
  populated pageを検証する。borrowed page view/穴/古い参照の拒否試験を追加。

これらのreaderは接続済みだが、production writer/syscall・全既存aliasの撤去・
snapshot回収・PMM返却前復元は未接続。起動時はsnapshot=nullのままで、既存のWB
経路を維持する。まだ新kernel配布や実機WC成功を主張しない。

次の準備処理も追加した（引き続きproduction writerには未接続）:

- 全ASの対象PFNに対するuser PTEを、呼び出し側が確保したscratchへ収集する。
  検査はPTE無変更で、容量不足、実行可能/global alias、対象PFNを含むlarge leafを
  拒否する。VMA/COW/DMA/所有権/supervisor alias検査を代替するものではない。
- Presentをatomic clearして現時点のPTEを保存し、TLB ack中に追加されたA/Dも
  公開時に保持する。PT解放・追加割り当てなしでPATのみ置換し、元のアクセス権と
  software bitを保つ。別processへの同一PFN mappingも対象にしたmock試験に成功。
- transitionの順序にpending公開、最終policy公開、最終reader rendezvousを明示した。
  最後のsnapshot差し替え後もackを待つため、以前のpending snapshotを読んでいる
  copy guardが残ったままmetadataを再利用しない。
- `zig build test` exit=0 (`.artifacts/ram-cache-alias-tests.log`)。

接続されたDisk 3 General UDisk / G: FAT32を読取確認した。KERNEL.ELF、BOOTFS.IMG、
DIAGBOOT.ELFのSHA256は上記配布版と一致。今回の未接続処理を修正版として配布せず、
USBは未変更。残作業はsupervisor alias、range単位のVMA/DMA検査、writer/syscall、
WB復元を保証するVMO retirement、Kobox CPA接続と実機描画/readback。

RAM VMO lifetimeの保護を追加し、全kernel testとlimine buildがexit=0:

- rootのcache stateをpublished snapshotから同期する。pending ownerは同期不可。
  この同期API自体はまだproduction writerから呼ばれていない。
- cached rootのfinal dropはref_count=0でもbackingと世代を保持し、outer retirement
  待ちにする。viewの最後のreleaseからの親dropと、free-listなしのdropも対象。
- partial unmap/direct owned-page releaseは非WB backingを解放せず、clearはWBを
  必須とする。retiringへの再retain/grow/store確保やcached backing置換も拒否。
- finishはcached/pending ownerが残っていれば失敗してページを保持する。callerが
  DMA失効・全alias/cache barrierを完了した後のみ、WBに戻してPMM返却・世代更新。
  これは状態管理のgateであり、WBをハードウェアに適用する処理そのものではない。
- `ram-cache-retirement-tests.log` / `ram-cache-retirement-build.log`。
  既存のNativeVmoSlot 64 byte制約も維持。USBは未更新。

supervisor identity splitの事前構築処理を追加:

- `memory/ram_cache_paging.zig`はcaller-ownedの未公開PTだけを構築する。2 MiB
  PAT→4 KiB PAT変換、RW/NX/software bit維持、global/user/未知のleaf拒否を試験。
  公開後のPT storageはkernelまたはprocess PT storeが保持する必要がある。
- 新しいnon-leaf PDEへdata leafのPWT/PCD/PATを流用しない。split中のhardware
  A/Dは全leafに保守的に保持する。実属性変更はその後の全CPU ack/drainが必須。
- 既存platformのstatic identity splitも同じ処理を利用。失敗前にsplit poolを消費
  しない。WC BAR preflightも同じsource検査を使い、commit時の新たな拒否を防ぐ。
  RAM writer用の動的PT予約・private PT lifetime接続はまだ未実装。
- 全test / limine build exit=0 (`ram-cache-paging-tests.log`, `ram-cache-paging-build.log`)。

この変更を含むkernelのQEMU起動回帰試験:

- `.artifacts/ram-cache-boot.lMPMz3/`、kernel SHA256
  `24c13139c4c48f794dfc6573f05607fa7a5dae94df6854b7f68ed2ad81d00849`。
  専用imageへ差し替え後、FATから読み戻して一致確認。実USBは未変更。
- KVM、Q35、32GiB、24CPU、AMD IOMMU、低位RAM384MiB、OVMF、qemu-xhci、
  USB boot媒体+USB keyboard/mouse、std VGAで起動。ashから
  `busybox echo cache_boot_ok`の出力・prompt復帰を`ash.png`で確認。
- この起動のnetdはNIC初期化でdetail=-19となった。SSH/ネットワーク成功の証拠には
  しない。QMP quit後process exit=0。std VGAはGOP/起動試験のみで、Mesa加速試験ではない。

未完了は引き続き、全supervisor aliasの動的準備、range単位VMA/DMA検査、
production writer/syscall、outer WB復元drainとKobox CPA callback、実機draw/readback。

追加の接続準備（まだproduction writerは未接続）:

- supervisor/user aliasを全ASから列挙し、kernel/private identityの動的PT予約と
  pointer重複排除を行う `ram_cache_vm_plan.zig` を追加した。予約失敗時は既存PDEを
  変更せず、withdraw後は追加確保しない。対象範囲単位のVMA/DMA検査も追加。
  `ram-cache-plan-tests.log` は325 tests成功、limine buildもexit=0。
- eager linear/scatter mappingも物理policyをページごとに継承する。AS lockを保持し、
  writerが全AS lockを取る契約の下で全範囲を検査してから公開する。pending、非WBの
  executable alias、明示cacheとの衝突は部分公開せず拒否する。
- WB/WC混在scatter、後半ページpending、exec/明示UC衝突時に先頭も未公開となる
  回帰試験を追加。`zig build test` exit=0 (`ram-cache-eager-tests.log`)。
- permission変更/remap、将来のVMA admission、writer/syscall、WB retirement drain、
  Kobox CPA接続は引き続き未完了。USBは旧配布版のまま。実機Mesa描画成功ではない。

その後、remap/mapOrRemap/installInvalidatedも全PFNのpolicyを事前検査し、
commitでページごとのPATを継承するよう接続した。remapのPT確保は先に済ませる。
protect/protectPresentは全対象を先に検査し、pending、非WB executable、
canonical RAM policyと既存PATの不一致を部分変更前に拒否する。BARのPATは保持し、
permission更新はCASでCPUのA/D更新を失わない。混在WB/WCのremap、pending時の
非変更、invalidated rangeの再公開、exec拒否とA/D保持を回帰試験に追加した。
残る将来VMA admission、production writer/syscall、WB retirementとCPA接続を
終えるまでは、実際のRAM属性変更を有効化しない。

VMA metadata admissionにも同じ物理policyを接続した。create/fixed fd replacement/
mprotect/fork/mremapの対象範囲をresolveし、pendingを拒否、非WBのprivate・exec・
shadow aliasを拒否する。共有非実行は許可し、BARの既存検査は変更しない。
max_protで将来execを許可していても、actual mprotect時に再検査する。
空policyのfast pathはpendingも空であることを確認し、公開snapshotを借用して
持ち出さない。range外のWCは無関係なWB executable mappingを阻害しない。
pending-only、mixed WB/WC、anonymous hole、shadowのfixture試験を追加した。
これはproduction writer/syscallとKobox CPA callbackの完成を意味しない。

内部commit bridge `memory/ram_cache_commit.zig` を追加した（syscall未公開）。
事前確保したplanとimmutable snapshotを受け取り、pending→全alias撤去→
物理copy window退役→全user CR3同期→kernel CR3同期+全CPU WBINVD→
新PTE公開→全user/kernel CR3同期→final policy→reader rendezvousを接続する。
scatter PFNは連続区間にまとめてcopy windowを退役させる。
pendingが対象全PFNだけを覆うこと、finalが対象外policyを変更しないこと、
旧PTE/大ページdirectoryのidentityとPATが変化していないことを公開前に検査する。
公開後は確保やエラー復帰をしない。VMO所有権・DMA/PAT/MTRR証明・全VM lock・
snapshot寿命とretirement状態更新は、今後接続するcallerの必須条件である。
mock architectureによる順序、連続PFN結合、pending不足、対象外policy変更、
WB復元の試験を含め328 tests成功 (`ram-cache-commit-tests.log`)。
実ハードウェアでこのcommit bridgeを実行した証拠ではなく、USBも未更新。

RAM requestの内部preflightを追加した。canonical anonymous VMOの世代付き所有権
からpopulated PFNを解決し、scratch上でsort/重複拒否、元の論理範囲に対する
VMA/DMA検査、全online CPUのPAT/MTRR証明を順に行う。
RAM証明はBAR証明と分離し、全対象pageでMTRR WBを必須とする（PAT0に戻しただけで
実効WBにならない範囲を排除）。WB/UC_MINUS/UCの固定slotは全CPU snapshotのvalid/
matchesで検査し、WCは全CPUに共通する実際のWC PAT slotを選ぶ。
snapshotで証明できない1MiB未満のfixed MTRR範囲、物理幅外、UC部分範囲、
不一致/不足CPU証拠を拒否。CPU構成/MSRはcommit完了まで不変であることがcaller条件。
`ram-cache-request-tests.log` は330 tests成功。まだFD権限判定を含むsyscall、
snapshot storage所有、VMO retirement WB復元とKobox CPAは未接続。

分散PFNのsnapshot準備をrequestに接続した。pendingは隣接PFNのみcoalesceし、
WB復元でも対象を保持する。finalは既存spanと変更spanのlinear mergeで構築し、
対象外属性の保持をcommit.Changeでも再検査する。出力は専用pending/final scratch
に限定し、入力や相互のstorage overlap、容量不足、所有者衝突を公開前に拒否する。
単一区間更新を繰り返す参照実装との256通りの比較を追加した。
snapshot storageの実際の確保・reader退役後の回収と、syscall/CPA接続はまだ残る。

page-backed snapshot storageを追加。header/snapshot/span配列をidentity範囲内の
PMM割当で保持し、pending/finalをcommit前に確保・構築する。サイズ算術はchecked、
writer metadataと対象PFNの重複は拒否する。published blockのdraft解放を拒否し、
PMM返却失敗時はintrusive reusable listに所有権を保持する。再利用時はsnapshotを
初期化し、PMMと同時所有しない。二つ目の確保失敗時の一つ目のrollbackと、
そのPMM返却まで失敗した場合の保持・再利用をfixtureで検査した。
このmoduleはdraft準備/rollbackのみ。published→retiredのreader barrierを伴う
実owner接続、syscall/CPA、実機描画はまだ未完了で、USBは未変更。

Storeにactive snapshotの所有を接続した。開始時にglobal publicationのpointerが
自身のactiveと一致することを検査し、draft pairを保護してnative commitへ渡す。
commitの公開前エラーでは旧activeを保持してdraftのみ回収、成功時は最後のreader
rendezvous完了後にfinalをactiveとして保持し、pendingと旧activeを回収する。
別writerのpublicationは拒否する。mockではfinal公開前後のfree発生がなく、
barrier完了後だけ回収すること、失敗時/繰り返し更新時の所有数を検査した。

canonical VMO lifetime ticketも事前検査に分離し、Store.applyのnative経路は
owner一致を確認してからcommitし、同じlock内でticket.finishをinfallibleに実行する。
したがって「属性公開成功後にcached state更新が失敗する」順序を作らない。
まだ外側のsyscall/lock取得、VM plan scratchの確保、final-drop WB復元drain、
Kobox CPAからの呼出しには未接続。USBは未更新、実機描画成功の証拠ではない。

内部 `Store.change` を追加し、canonical VMO範囲→PFN解決/事前証明→
全alias計数/収集→動的PT plan準備→snapshot commit→VMO lifetime更新を接続した。
PFN/source/leaf/split scratchはchecked typed layoutでページ確保し、失敗時も
plan reservationをscratchより先に解放する。公開済みsplit PTは既存の所有権移管
規則に従い保持する。scratchとsplit PT自身が変更対象PFNに入る場合も拒否。
この経路の空要求テストはstate参照前の拒否と全呼出し経路のcompile確認であり、
native WBINVD/実機属性変更の成功を示すものではない。syscall公開とretirement
WB復元、Kobox CPA接続は引き続き未完了。

内部 `Store.retire` を追加した。ref_count=0/retiringのcanonical ownerを検査し、
そのownerの非WB policy spanだけを選んで、全PFNが残存backingに含まれることを
一回のbacking scan+二分探索で検証する。WBのPAT/MTRR証明、通常と共通のalias
planとcommitを経てからfinishでPMMへ返す。失敗時はretiring ownerを保持する。
またfinishがPMM返却失敗ページを残したままslotをclearする問題を修正し、
残存backingが空になるまでretirement count/世代/所有権を維持する。
全返却失敗、一部返却成功、再試行時の二重返却防止をstate試験に追加した。
この経路もまだ外側の解放drain/syscallには未接続で、実機復元の証拠ではない。

外側の解放drainを `syscall/ram_cache.zig` に接続した。通常syscall終了、
current-thread離脱、外部principal teardownの既存device retirement後に、
KernelStateと全VM lockの下でretiring ownerを一巡する。失敗ownerを保持しつつ
後続ownerも処理し、同一失敗のログ連打を抑える。これは内部経路であり、
属性変更用FD syscallはまだ公開していない。

DMAの強制解放ではFD pinが消えてもIOMMU quarantine ledgerにPFNが残り得る。
通常changeとWB retirementの両方でこのledgerを事前検査し、隔離中は属性自体を
変更しない。PMMの再利用拒否だけでは、先行するcache transitionを防げないため。
`ram-cache-drain-tests.log` は340/340成功、limineビルドも成功。
この試験はnative cache変更の実行成功やMesa描画の証拠ではない。

次の有効化条件は、DMA capability移譲/owner破棄経路と実page-table parentの
対応を検査した上でのFD API接続、その後のKobox CPA接続である。現在の
`protect_direct_range` はRWXだけを反映しており、Linux PAT変更はまだnative RAM
policyに届かない。CPAのWBINVDを単に抑止するだけではこの不足を解決しない。

DMA lifetime監査では、pinned objectのFD/IPC移譲禁止とowner終了前の全参照失効を
確認した。一方、非IOMMU経路の `releaseDmaIova` はhardware drainを行わないため、
新規change/retireはactive IOMMUを必要とし、未対応経路では
`UntranslatedDmaUnsupported` を変更前に返す。これは今回の変換済みAMDGPU経路を
有効にするための証明範囲であり、一般的な非IOMMU DMAの安全性を主張しない。

process alias列挙の前にlive CR3/PML4/PDP/PD/PTの親子関係を検査するようにした。
CR3の低位control/PCID bitsだけを除外し、upper rootを含む未知のroot、detachedな
tracked table、異なる物理pointer/cache/permissionを拒否する。既知のshared
kernel-image rootとidentity PDだけをplatform accessorで認め、未知tableは辿らない。
non-leaf Accessed bitの更新は許す。supervisor identity splitのNXも保持する。
通常カーネルrootそのものの網羅性確認とFD syscall/CPA接続はまだ残る。

親pointer差替え、detached PT、親cache bit/huge bit不一致で列挙前に失敗する試験と、
supervisor RO/RW × NX有無のPTを繰り返し列挙できる試験を追加した。
後者で、split時に保持したROを次回だけ不正として拒否する不整合も修正した。
`ram-cache-topology-tests.log` は344/344成功、同変更のlimineビルドも成功。
USBは未更新であり、この結果をnative属性変更や実機Mesa描画の合格とはしない。

native `Store.applyRequest` の先頭にもkernel root検査を追加した。CR3が既知の
PML4を指し、presentなPML4がlow identity/high MMIO/high imageの既知PDPだけを
参照し、それらのPDPが所定の静的PDへ接続されていることを検査する。
不明なroot、PD pointer差替え、parent cache bit差異は未知pointerを辿る前に拒否。
これはroot階層の検査であり、動的identity PTの所有記録を含む下位階層全体の
証明を代替しない。下位の所有検査が揃うまではFD APIを公開しない。

FD APIはcanonical VMOのoffset/lengthとcache modeを受け、専用cache-policy権限と
map-write権限を要求する方針。物理属性は他の共有aliasにも影響するため、既存の
write/resize権限から暗黙に昇格させない。page viewには新権限を渡さず、ABI番号と
権限bitは論理的位置へ挿入して全producer/consumerを再ビルドする（未実装）。

## 2026-09-28 実機 Mesa の停止点と再現範囲

`run-20260927T172512Z-fsjnK2` では実 AMD renderer に到達したが、最初の
2 MiB GTT BO (`domains=2`, `flags=4`) 確保で止まった。Kobox2 の RAM ログは
Linux RAM VMO offset `0x4400000`、長さ `0x200000` の UC−属性変更の入口
(`stage=0`)まで記録し、復帰 (`stage=1`) は記録していない。gpud は15秒後に
transport timeoutを報告した。描画成功でも、属性変更処理が永久停止した証拠でもない。
安全停止は確認できず、監視スクリプトはACPI電源断を要求した。

既存のnative属性試験を、Kobox2と同じ384 MiB VMO、68 MiBの既存populate、
その後の2 MiB範囲変更へ寄せた。AMD IOMMU・32 GiB・24 CPU・xAPICのQEMUで
主マップを含む64 alias、20 clone、同範囲への並行readを伴うWB→UC−→WBが通った。
`native-ram-cache-policy-alias64-clone20-readers-xapic/serial.log` に全commit段階と
PASSがある。従って単純なVMO長・populate・alias数・read競合だけでは再現しない。
実機のDMA/IOMMU状態、物理APICとlive service競合はこのfixtureでは再現していない。
原因を確定できるまで、USB上のカーネルとbootfsはこの追試では更新しない。

## 2026-09-28 live 環境での属性変更切り分け

監視付きの再試験 `run-20260927T173951Z-FblPqO` でも、実 AMD renderer と
EGL 1.5 には到達したが、offset `0x4000000` / 2 MiB の最初の UC−変更は
Kobox2 の `stage=0` の後に `stage=1` を返さなかった。gpud の ioctl transport は
15秒後に timeout し、sandbox 停止後に ACPI 電源断を要求した。これは Mesa の
shader/CS が成功したことを意味しない。

GPU なしの native 起動 fixture は散在 PFN（対象ページの間に最大128ページ分の
別割当を挟む）にも拡張した。32 GiB・24 CPU・AMD IOMMU・xAPIC・64 alias・
20 clone process・並行 reader で WB→UC−→WB は通り、起動から PASS までは
再実行時に約6.1秒だった。記録は
`native-ram-cache-policy-alias64-clone20-readers-scatter128-xapic/serial.log`。
ただしこの fixture は live サービス群の実際の VM 配置を再現しない。

live の ash は Linux 互換実行経路であり、そこから native fixture の raw syscall を
直接実行すると syscall 56 は Linux `clone` として解釈される。したがってその結果を
native VMO の失敗とは扱わない。代わりに別ビルドの Kobox2 sandbox を RAM 転送し、
Linux モジュール・GPU ドライバ起動前に同じ 384 MiB VMO 内の 2 MiB を一度だけ
変更する切り分けを実機で行った (`run-20260927T175836Z-TJzRrU`)。
`stage=0x10` (fixture開始) と `stage=0` (native syscall直前) は出たが、復帰側は
出ず、sandbox は直後に終了した。GPU ドライバ自体を原因とみなす根拠はなくなったが、
終了が native fault、kernel kill、または別の失敗のどれかは現行の RAM ログだけでは
まだ確定しない。推測で全CPU同期のackを省略したり timeout 成功にしたりしない。
gpud の次回ビルドでは cleanup の KILL 前に process wait の終了状態を RAM ログに
残す。試験専用sandboxは通常パッケージと分離し、通常ビルドは変更しない。

更新した `BOOTFS.IMG` を USB に読み戻し照合してから冷起動し、
`run-20260927T181057Z-sx7xzc` で同じ GPU 前 preflight を再実行した。
`stage=0x10` と `stage=0` の約29 ms後に、sandbox は `image.c:427` の
失敗判定から lifecycle failure を送って終了した。これは native syscall の
戻り後の経路であり、少なくともこの preflight は無期限停止していない。
GPU driver/module はまだロードしていない。gpud は DMA quarantine と process
stop を完了し、host は ACPI 電源断を要求した。

この試験ビルドは `memory_set_cache` の native status を `1` に丸め、失敗時の
`PH_CHECK` がさらに `0` にしていたため、記録された `errno=-14` は元の失敗値
ではない。また GPU RAM ログは16メッセージの IPC キューに対して固定幅の長い
レコードを連続送信し、復帰側の断片が落ち得る。接続側で native status を保持し、
ログ値を最短の16進数にしてから次回の実機試験に臨む。kernel/ABI は変更しない。

続く `run-20260927T181508Z-mBQ77W` では同じ USB から冷起動し、変更した
試験用sandboxだけを RAM へ転送した。Linux/GPU driver より前の 2 MiB UC−
変更は `stage=0x1 status=0x4` を返した。これは native `MAP` status であり、
`OutOfFreePages` などの `ALLOC` status ではない。失敗は約18 ms後に処理され、
DMA quarantine・sandbox process stop・ACPI電源断を要求した。
現行 syscall は alias 検証、PAT/MTRR証明、所有権など多数の失敗を `MAP` に
まとめるため、この値だけでどの内部検証が拒否したかは確定しない。

デバッグ版kernel log ringを接続した live image を作り、QEMUでは
`/run/pacha/kernel.log` にkernel行を確認し、ユーザーのserial出力は混入しないことを
確認した。実機の拒否理由は未取得。次の監視付き試験では host script が停止後・
ACPI電源断前に同ファイルを `kernel.log` として回収する。ring はRAMだけにあり、
この回収が失敗した場合に `MAP` の内部理由を推測で埋めない。

## 2026-09-28 WC GTT の DRM page view

静的監査で、AMDGPU の `AMDGPU_GEM_CREATE_CPU_GTT_USWC` は TTM の
`ttm_write_combined` を選び、TTM pool はページ確保時に
`set_pages_array_wc()` を実行することを確認した。従来の Kobox2 DRM mapping は
`TTM_PL_TT` の `ttm_cached` だけを許し、この成功済み WC GTT を後段の
GEM_MMAP で拒否していた。provider は WB/WC の TTM ページを区別して返し、
native adapter は WC RAM も parent RAM VMO の PAGE_VIEW として公開する。
native の物理 cache-policy snapshot が新しい page view の PTE に属性を継承するため、
別個の WB alias は作らない。`ttm_uncached` は Linux の UC-minus と現在の
DRM mapping wire の strong UC を同一視できないため引き続き拒否する。
PRIME の WB RAM 限定も変更しない。

これは属性変更が通った後に必要な接続であって、現在の実機の UC-minus 拒否を
解消した証拠ではない。Kobox2 mapping module のビルド、RAM WB/WC page-view
公開と不正 cache/範囲拒否の単体試験、Mesa static gate は成功。PCIなしの
21モジュール load/unload は `cleanup=0`、固定 firmware の正負試験も通った。
実機描画と shader readback は未確認。

通常パッケージと分離した `--cache-preflight` パッケージも現ソースから再生成した。
両者の `drm_mapping.ko` と `manifest.bin` は同一ハッシュで、前者の sandbox
だけが追加診断を含む。`pacgo build userland --no-rootfs` で通常アーカイブを
正式な成果物スロットへ移してから bootfs を生成したため、新しい
`BOOTFS.IMG` は `ae8bd079e1996f6fdf1c33359cf7747090bbbeb31b39326984ad1d32138f98f1`。
Disk 3 General UDisk 上では旧版
`7107241418f2c9aa828b79085be838bb05140b97371dfba3d21232df6cc70eae`
を `BOOTFS.PREWCGTT.IMG` に残し、新版だけを読み戻し照合した。

## 2026-09-28 実機の RAM 属性拒否

更新済み USB からの冷起動と SSH を確認し、`--cache-preflight` パッケージを
RAM に転送して、一回の監視付き試験を実施した。driver/module 起動前に、
64 MiB offset の最初と 2 MiB 先の最後、それぞれ独立した 4 KiB の
WB→UC-minus が `status=0x4` で失敗した。GPU の probe には進んでいない。
`gpud` の sandbox 停止を記録し、kernel ring を回収した後に ACPI 電源断を
要求した。証拠は
`.artifacts/amdgpu-inspections/run-20260927T193359Z-UVoat0/` に保存した。

回収した kernel ring は両ページとも
`ram-cache: syscall refused reason=RamNotWriteBack` と報告した。
従って、これは 2 MiB 一括変更の metadata 容量や TLB 同期の問題より前の、
物理ページに対する MTRR/PAT 証明の拒否である。VMO backing は通常の PMM
割当であり、現実装は 4 GiB 以上の物理ページを優先する。一方、
`mtrr.CacheSnapshot.uniformMtrrType()` は可変 MTRR に一致しない場合
`MTRRdefType[Type]` を使うが、AMD の
`SYSCFG[Tom2ForceMemTypeWB]` と `TOP_MEM2` は取得・評価していない。
AMD APM は、この bit が有効なら 4 GiB から TOP_MEM2 までの既定型を
WB にすると定義している。Linux もこの AMD 固有の既定 WB 領域を MTRR の
メモリ被覆判定で扱う。この欠落は実機での拒否と整合するが、対象 PFN、
SYSCFG、TOP_MEM2 の実値はまだ採取していないため、原因確定とはしない。
MTRR の WB 要件を消す、UC 扱いのページを無条件で通す、または Linux/Mesa
ソースを変更する方法は採らない。

次は kernel の権限下で対象 PFN と AMD cache snapshot を一回だけ既存の
RAM ring に診断記録し、条件が合致した場合に限り AMD の実効 MTRR 判定へ
TOP_MEM2 の既定 WB 規則を加える。可変 MTRR の上書き、4 GiB/TOP_MEM2 境界、
CPU 間 snapshot 一致、および bit 無効時の拒否を単体試験で維持する。
これは CPU MSR と物理 cache 属性を扱うため、userland では安全に判断できない。
参照: [AMD64 APM Volume 2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)、
[Linux の AMD 特別 MTRR 判定](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/7acf90feab8b009fde7def08ff2c622d0f10e99f/arch/x86/kernel/cpu/mtrr/cleanup.c)、
[Linux の TOP_MEM2 cache map](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/7acf90feab8b009fde7def08ff2c622d0f10e99f/arch/x86/kernel/cpu/mtrr/generic.c)。

カーネル変更の範囲はサブエージェントに審査してもらい、MSR/PFN を扱う
最小の変更として承認を得た。`CacheSnapshot` は物理 AMD CPU の
`SYSCFG[21:22]` と有効な `TOP_MEM2` を取得し、CPU 間でその有効属性を
照合する。可変 MTRR に一致しないページだけで TOM2 の既定 WB を適用し、
可変 UC 等の上書きは保持する。拒否時には対象 PFN、MTRR 型、SYSCFG、
TOP_MEM2、MTRRdefType を既存のデバッグ ring に記録する。仮想化環境では
追加 AMD MSR の読み出しをしないため、TOM2 の例外を推測しない。
単体の境界・override・CPU 間不一致テスト、`zig build test`、
`zig build kernel`、QEMU の 2 MiB WB→UC−→WB native smoke は成功。
実機の該当 PFN と TOM2 有効性、および Mesa draw/readback は再試験待ち。

## 2026-09-28 実機 Mesa render 再試験

新しい debug-ring kernel を Disk 3 の `KERNEL.ELF` に読み戻し照合で更新し、
旧版は `KERNEL.PRETOM2.ELF` に保存した。実機の PCI なし preflight は
2 MiB の UC-minus 往復を完了し、以前の `RamNotWriteBack` は再発しなかった。
PCI probe では DRM render node が開き、Mesa EGL 1.5 はソフトウェアではない
`AMD Radeon Graphics (radeonsi, gfx1200, LLVM 20.1.8, DRM 3.64, 6.12.0)`
を報告した。したがって firmware、Linux driver bind、DRM open、GBM/EGL、
GL context まで実機で到達した。ただし描画/readback はまだ成功していない。

最初の 2 MiB GTT BO 確保で `set_pages_array_wc()` に伴う direct-map cache
変更が拒否された。kernel ring は
`ram-cache: syscall refused reason=DmaPinned offset=0x4200000 pages=512 mode=uc_minus`
と記録し、Kobox2 側は同範囲に direct DMA mapping が 1 件あると報告した。
Linux の `ttm_pool.c` は今回の経路でページごとに `dma_map_page()` を行った
後、確保バッチ末尾に `set_pages_array_wc()` を呼ぶ。現行 Kobox2 は DMA map
を即時に native DMA mapping FD として登録し、native cache-policy writer は
該当ページの DMA pin を拒否するため、この順序で衝突する。Linux/AMDGPU や
Mesa のソースは変更しない。`scattered_dma=1` は診断 inventory 内の全
page-view mapping 件数であり、この範囲との重なりは証明しない。

今回は失敗した Linux CPA 後に render job が時間制限で終了し、`gpud`
の stop 確認は所定時間内に取れなかった。host は kernel ring を取得後に
ACPI 電源断を要求し、SSH は切断された。追加 probe はしない。証拠は
`.artifacts/amdgpu-inspections/run-20260928T045358Z-aaRimG/`。
次は DMA pin と RAM cache transition の整合を、デバイスが当該 IOVA を
使用中かどうかという安全条件まで含めて設計し、同じ監視付き手順で再試験する。

### streaming DMA pin と属性変更の分離（実機検証）

Linux DMA API では `dma_map_page()` が返した IOVA は直ちに有効なので、
Kobox2 側で DMA mapping FD の公開を一律に遅らせる方法は採らない。
今回の TTM fresh allocation では `ttm_pool_alloc()` 内で DMA map の後に
`set_pages_array_wc()` が終わり、その後の GART bind で初めて GPU にページを
渡す。従ってこの経路に限り、属性変更中に対象ページへのデバイス DMA は
進行していない。一般の DMA mapping が常にアイドルであるとは主張しない。

kernel の RAM cache-policy 検証では streaming `.dma_mapping` pin を
ページ寿命・VA/PFN/IOVA 固定として扱い、属性変更だけは妨げない。
VMA の munmap/mremap、VMO の解放を防ぐ既存の pin 検査は変更しない。
`.dma_buffer` と `.mmio_region` の pin は引き続き属性変更を拒否する。
所有権、全 alias、PAT/MTRR、translated IOMMU と quarantine の条件も維持する。
新しい ABI や GPU 固有例外はない。この kernel 変更は AGENTS.md に従い、
kernel の責務と狭い適用範囲をサブエージェントに説明し、承認後に行った。

`zig build test`、`zig build kernel`、GPU なしの QEMU 2 MiB
WB→UC-minus→WB 試験は成功した。単体試験では streaming mapping の存在下で
属性検証が成功し、同じ pin が VMA 再利用を引き続き防ぐこと、別種の pin
による拒否と検証失敗時の WB 状態保持を確認した。実機の shader draw と
readback、GPU の正常停止はまだ未確認である。

この修正を含む kernel は debug ring 有効で再ビルドし、SHA-256 は
`23e5d909f9ad081d36f263a759f6ecc8b08b80ce7e1294655a1fcec3fa2562cb`。
Disk 3 `General UDisk` の既存 `KERNEL.ELF` が直前版のハッシュと一致することを
確認して `KERNEL.PREDMAPIN.ELF` に保存し、新しい `KERNEL.ELF` の読み戻しが
ビルド成果物と一致した。`BOOTFS.IMG` は
`ae8bd079e1996f6fdf1c33359cf7747090bbbeb31b39326984ad1d32138f98f1`
のまま変更していない。

再試験 `run-20260928T050523Z-hV6tPt` では、PCI なしの 2 MiB 往復に加え、
実 GPU の GTT 確保で UC-minus→WC の両属性変更が kernel ring 上で完了した。
`DmaPinned` の拒否は再発していない。GBM/EGL と実 `radeonsi gfx1200`
renderer の取得も成功した。ただし、その直後から render client が進まず、
25 秒の guest 上限で終了した。shader draw/readback は未達で、gpud の stop は
`status=-110`、host は kernel ring を回収して ACPI 電源断を要求した。
現時点の service.log 回収は末尾の cleanup ログで埋まり、描画停止直前の
Kobox2 状態は分からない。したがってキャッシュ属性変更自体は通過したが、
次の停滞点を断定しない。次回は Mesa client の GL 操作境界と service.log の
連番を host 側へ逐次保存し、同じ監視・電源断手順で原因を確定する。

続く `run-20260928T050953Z-mkShiK` は render client に境界ログを加え、
host が service.log の連番を逐次回収した。シェーダーの compile/link と
`glDrawArrays` の送信までは成功し、直後の AMDGPU GEM CPU mmap で
Kobox2 `map failure stage=0x5 status=0x1 handle=0xa bytes=0x200000` が出た。
これは native attachment 公開時の INVALID 応答であり、
先の WC 属性変更自体は完了している。sandbox は終了して gpud は
`status=-71`、host は kernel ring 回収後に ACPI 電源断を要求した。
draw の fence 完了と readback は確認できていない。

既存の kernel syscall は入力、権限、親 VMO、ページ解決、view への格納を
同じ INVALID 値へ畳む。WC 親からの page view 作成は state 単体試験で成功し、
WC 一般の禁止ではない。kernel 内部の拒否位置は userland の既存情報から
確定できないため、AGENTS.md の kernel 編集審査を経て
`createPageViewVmoFd` の失敗時だけ既存 debug ring に段階・index・論理親
page・理由を最大1行かつ rate limit 付きで残す。ABI、戻り値、検証、
rollback は変更しない。`zig build test` と debug-ring kernel ビルドは成功。
新しい kernel は SHA-256
`ff14a3c6514b5d7bc6b0cb36209e66da6aaedd804a6bc73267212700f34cbde2`
で Disk 3 の `KERNEL.ELF` に保存し、旧版を
`KERNEL.PREPAGEVIEW.ELF` に残した。再接続時の USB 上の読み戻しでも同じ
hash を確認した。state 単体試験は WC 親の 512 ページ view に拡張して成功し、
GPU なし QEMU でも native syscall 経由の WC view 作成・mmap・内容確認・
解放まで成功した。従って、2 MiB サイズや WC 親だけで必ず失敗するわけではない。

実機 `run-20260928T052539Z-ECYOxC` では再び実 `radeonsi gfx1200`、
shader compile/link、`draw-submitted` に達し、WC 切替は完了した。
同じ `map failure stage=0x5 status=0x1 handle=0xa bytes=0x200000` で停止したが、
取得した kernel ring に新しい page-view 拒否行がなかった。これは失敗の
内部理由が未確定であり、ページ欠損などに決め打ちできないことを意味する。
GPU は監視付き試験の終了時に stop と ACPI 電源断を要求した。

次の bootfs では、Kobox2 が失敗時だけ parent FD、権限、cache、件数と
先頭・中間・末尾の親ページ番号を RAM service log に残す。`logd` も debug
ring の競合による欠落件数を `kernel.log` に表示する。Linux/Mesa 本体と
ABI は無変更。GCC/Clang の Kobox2 fault-log unit と gpud GPU query unit は
成功した。専用 `BOOTFS.IMG` は SHA-256
`d3549d5040320076ac92f28f3b3a76616ce15ead9eb3bd2cf92f7797a74c574d`
で Disk 3 に読み戻し照合して保存し、旧版を `BOOTFS.PREPAGEVIEW.IMG` に
残した。古い複製 QEMU image は init app の世代違いで初回停止したが、
現行 init app に揃えた再試験では live shell 起動まで到達した。実機の
shader fence/readback は依然として未達である。

続く `run-20260928T053818Z-HeQSR5` でも同じ stage 5 で停止したが、
Kobox2 の RAM page-view 専用失敗ログ、kernel の page-view 拒否ログ、
debug-ring 欠落マーカーのいずれもなかった。上の page-view と断定した
記述は誤りである。stage 5 は BAR export と RAM page-view の共通段階で、
今回の証拠は BAR export 側を指す。ただし、BAR 範囲外の PFN か native
`DERIVE_MMIO` の拒否かはまだ区別できない。

Linux AMDGPU の通常の CPU fault は `amdgpu_bo_fault_reserve_notify()` で
non-visible VRAM を visible VRAM/GTT に移してから mapping する。一方、
Kobox2 の `drm_mapping.c` は VRAM PFN を取って BO を pin し、後段の native
BAR 照合に任せていた。pin 後は移動できないため、Kobox2 側で全 PFN が同一
PCI BAR に収まるかを pin 前に検査し、収まらない未 pin の BO を TTM GTT
placement へ移して fence を待つよう変更した。既に pin された BO は移動を
強制せず拒否する。native adapter は BAR export 失敗を引数不正・BAR 範囲外・
syscall 拒否の三種で RAM log に区別する。Linux AMDGPU と Mesa 本体、ABI、
kernel は変更していない。Kobox2 module の strict modpost build、PCI/unit
および fault-log GCC/Clang unit、gpud query unit は通過。実機での
shader fence/readback はこの変更では未検証であり、成功扱いしない。

更新した optional bootfs の SHA-256 は
`cf7cb0f747ec9b205a08b23232261e660899ff0314042feddbb149f70c97e631`。
General UDisk（Disk 3）の `BOOTFS.IMG` に書き込み、媒体上の読み戻しでも
同じ hash を確認した。旧版は `BOOTFS.PREBAR.IMG` に残した。現行 kernel と
init app、同じ bootfs byte 列を入れた QEMU UEFI/KVM image は
`live services ready`、`/dev/pts/0` 接続、root 起動まで到達した。
QEMU に実 AMDGPU はなく、これは実機 GPU probe や shader readback の証拠ではない。

### 2026-09-28: 実 AMDGPU の headless Mesa 描画

実機 `run-20260928T060356Z-mGznTw` で、Mesa 25.1.9 が
`AMD Radeon Graphics (radeonsi, gfx1200, LLVM 20.1.8, DRM 3.64, 6.12.0)`
を選択した。software/virgl renderer は試験プログラムが拒否する。
GBM/EGL context、shader compile/link、draw、`glFinish`、`glReadPixels` が
完了し、RGBA `64,128,191,255` を確認した。surfaceless FBO なので KMS と
画面出力はまだ対象外。監視温度は約43–44℃で、試験後に ACPI 電源断を要求した。

ただし停止ログには sandbox 側 `result=-16` (`-EBUSY`) があり、従来の
`gpud: PCI DRM probe stopped status=0` は controller の資源退役だけを
報告して、sandbox の異常終了を隠していた。描画成功と正常停止を混同しない。
`gpud` は quiesce 失敗を退役後の最終 status に残し、Kobox2 は cleanup の
quiesce・module unload・device・PCI/IRQ/DMA/BAR・firmware 各段階の失敗を
RAM service log に出す。これは失敗時限定の診断で、Linux/Mesa 本体、kernel、
ABI は変更しない。更新版の PCI なし load/unload は 21/21、cleanup=0。

更新した optional `BOOTFS.IMG` は SHA-256
`3db31340746508f1ca241beabccb6b4b2827d28151065e07ea689c7977d1c116`。
Disk 3 `General UDisk` の旧版 SHA-256
`cf7cb0f747ec9b205a08b23232261e660899ff0314042feddbb149f70c97e631`
を `BOOTFS.PREDRAW.IMG` に残し、新版を USB 上で読み戻し照合した。
新診断での実機停止結果は未検証であり、正常停止とはまだ記載しない。

続く実機 `run-20260928T062954Z-f5rrTf` でも実 `radeonsi gfx1200` による
shader 描画と RGBA `64,128,191,255` の readback は成功した。一方、
`gpud` の最終停止 status は `-10` となり、停止失敗を隠さなくなった。
停止直前の Kobox2 診断は PCI BAR `0xfce00000` / 512 KiB に
`aliases=128` が残ることを示した。通常の BAR 登録解除はこの生きた alias を
`-EBUSY` で拒否するのが正しい。Linux AMDGPU 本体を改変せず、全 DRM file と
module の終了後、最終 device generation の回収時だけ、所有 BAR に残る
`VM_IOREMAP` VMA を Linux の `iounmap` で解除する処理を Kobox2 に追加した。
host lease だけを消す処理ではなく、PTE と cache/memtype の Linux 側 teardown
を通す。通常の PCI remove が使用中 alias を拒否する回帰条件は維持した。

PCI gate の通常 remove 拒否・最終 orphan 回収・fault 再確認・二世代再利用、
AMDGPU 固定 Clang build と strict modpost、PCI なし 21/21 module load/unload
`cleanup=0` が通過した。新 optional bootfs SHA-256
`ea9d1e1e2ca5027e0ff6ba7737a88c1b1c739a5b4491a7a36608588ec9bbc9f4`
を Disk 3 の `BOOTFS.IMG` に書き、媒体上で読み戻し照合した。前版は
`BOOTFS.PREORPHAN.IMG` に残した。

実機 `run-20260928T064340Z-43NhEr` では bootfs 内の driver archive、Mesa、
gpud の各ハッシュを SSH で照合した。GPU は 43–45℃で監視し、同じ実
`radeonsi gfx1200` の GBM/EGL、shader draw、`glFinish`、readback が成功。
停止時に `kobox-mmio: retired orphan ioremap ... aliases=128->0`、続いて
`gpud: PCI DRM probe stopped status=0` を確認した。host 側試験は status 0 で
終了し、kernel ring を保存して ACPI 電源断を要求した。表示/KMS や長時間運用は
この headless 描画試験には含まれない。
