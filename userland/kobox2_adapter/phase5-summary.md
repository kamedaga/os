# PachaOS adapter 項目5 完了記録

## 完了範囲

項目5では、Linux版と同じkobox2基盤をPachaOS上の別process sandboxへ接続した。
製品経路は次の1本であり、adapter内に別のLinux application実行環境は持たない。

```text
Linux application / Mesa
  -> 既存LPR（Linux syscall・FD semantics）
  -> gpud（kobox2 controller、DRM service、generation owner）
  -> PachaOS native IPC / shared VMO
  -> kobox2 sandbox（Linux DRM core・driver）
  -> virtio-gpu
```

接続済みの範囲は以下。

- RAM、native thread、TLS、非同期通知、時計、logical CPUを接続し、正規
  `start_kernel()`、SMP、initcall、第1章・第2章core Gateを実行する。
- packageを検証して別processのGPL sandboxへ渡し、READY、QUIESCE、終了、資源回収を
  kobox2 controllerのactionとして処理する。
- PCI config/BAR、予約済み領域へのMMIO、DMA/SG、IOMMU domain停止、IRQ/MSI-Xを
  PachaOS capabilityへ接続する。
- generationごとにmanagement channelとGPU split virtqueue VMOを作り、DRM
  `SESSION_OPEN`、`VERSION`、`GET_CAP`、`SESSION_CLOSE`を実Linux DRM fileへ到達させる。
- LPRのDRM `open/ioctl/dup/close`をgpudへ接続する。fork・FD転送は同じopen-file
  descriptionの参照を増やし、元FDのclose後も複製FDからioctlできる。
- 各open/dupの通知capabilityをgpudが保持する。clientの`SIGKILL`によるHANGUPでその参照を
  削除し、最後の参照ならsandboxの実DRM fileを同期closeする。その後の別clientの
  `open/ioctl/close`も成功する。
- sandbox強制終了時はDMA停止、DRM/VMO/IPC資源のrevoke、process reapを終えてから
  generationを更新する。旧handleを拒否し、新sandboxへの再openを許可する。

## 責務と配置

- `kobox2/`はOS非依存のcontroller、closure、GPU protocol、virtqueue engineを持つ。
- `kobox2/linux-sandbox/`はGPLのLinux core、module loader、実DRM file操作を持つ。
- `userland/kobox2_adapter/`はPachaOS ABI、native capability、thread/notification、VMO、
  DMA、IRQへの変換を持つ。PachaOS固有コードなのでkobox2共通repoには置かない。
- `userland/gpud/`はPachaOS上のサービスownerで、sandbox起動、generation、DRM handle、
  LPR endpointを管理する。Linux applicationとMesaは既存LPRで動く。

## FD寿命

gpudのDRM handleはopen-file descriptionを表す。管理はhandleごとの参照数と、参照ごとの
通知capabilityだけで行う。

1. `OPEN_NODE`でsandbox sessionと元FD用の通知capabilityを登録する。
2. `dup`またはfork時のFD転送で同じhandleの参照を1つ増やす。転送leaseがある場合は
   その通知capabilityも登録する。
3. 明示closeまたは通知HANGUPで参照を1つ減らす。
4. 最後の参照かつin-flight ioctlが0になった時だけ`SESSION_CLOSE`を送り、成功応答後に
   handleを解放する。

service endpoint自体をclientごとの擬似connectionで包まず、filedから受け取った
process-lifetime endpointをgenerationのDRM serviceへ直接bindする。client間share表は持たない。
要求payload内の番号を寿命の根拠にせず、open/dup時に転送された通知capabilityを根拠にする。

## 最終Gate

repo rootから実行する。

```sh
bash tests/run-gpud-drm-files-unit.sh
bash tests/run-native-gpud-core.sh --device
tests/run-lpr-qemu-drm-card0-smoke.sh
tests/run-lpr-qemu-gpud-restart-smoke.sh
```

`run-native-gpud-core.sh --device`は2 generationについて正規boot/SMP、第1章、第2章、
package/module lifecycle、PCI/DMA/IRQ、実DRM file、GPU session/query、終了後回収を必須にする。

通常LPR Gateの成功markerは以下。

```text
DRM_RENDER_OK name=virtio_gpu version=0.1.0 prime=3 \
  dup=1 fork_lease=1 last_close=1 client_kill=1 reopen_client=1
[gpud] drm client-hangup ... backend-close=1
```

restart Gateは追加で以下を必須にする。

```text
[gpud] sandbox-force-killed generation=1
[gpud] generation=1 retired next=2
[gpud] ready generation=2
GPUD_RESTART_CLIENT_OK old_generation_retired=1 reopen=1
```

最終QEMU実行では、DMARの単一segment-0 DRHDが列挙したtype-1 endpoint scopeと
present PCI functionを照合し、11 functionすべてに11 contextを作成した上で
`vtd: mode=translated ... active=1`を確認した。scope外のfunctionが1つでもある場合は
translationを有効化しない。

最終実行ログは`.artifacts/tests/native-gpud-device/serial.log`、
`.artifacts/test-results/phase5-final/normal-serial.log`、
`.artifacts/test-results/phase5-final/normal-console.log`、
`.artifacts/test-results/gpud-restart/`にある。

共通側の回帰確認では、kernel unit test 166件、kobox2/Linux sandboxの通常CTest 48件、
GCC/Clang sanitizer構成のprotocol/controller CTest各7件を通した。ローカルQEMU 8.2.2が
対応しない`-accel qtest`専用4件はこの48件から分離し、PachaOS上のdevice・revoke・再起動は
上記のnative/QEMU Gateで検証した。新規protocol headerとx86_64 backend libraryは
`cmake --install`の出力にも含まれることを確認した。

## 整理内容

- productionで共有endpointを包むだけだった`drm_connection.c/h`を削除し、DRM serviceへ統合した。
- productionで全要求が同一IDになっていたclient/share台帳を削除し、実際のFD leaseに対応する
  handle参照数と通知watchへ縮小した。
- 実QEMU Gateと重複していた擬似channel/HANGUP native fixtureを削除した。
- 途中経過と古い未完了判定を含む16個の検証MDを本書へ統合した。

## 次の項目

項目5はDRM service基盤と障害復帰までを対象とする。実MesaによるVirGL描画、fence、画素検証、
KMS表示、複数描画clientは項目6で行う。
