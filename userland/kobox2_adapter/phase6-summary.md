# PachaOS VirGL（項目6）

## 結果

PachaOS上の経路を次の一本に統合した。

```text
Linux application / Mesa
  -> LPR
  -> gpud（DRM service・sandbox controller・generation owner）
  -> kobox2 sandbox（Linux DRM core・virtio-gpu）
  -> QEMU virtio-gpu-gl
  -> host VirGL renderer（D3D12）
```

旧`drmd`のsource、build定義、rootfs配置、専用unit／QEMU runnerは削除した。
互換serviceやfallbackは置かず、filedのDRM endpointはgpudを直接公開する。

## 接続した範囲

- LPRのDRM `open/ioctl/mmap/read/poll/dup/close`をgpudへ接続した。DRM eventは
  sandbox内のLinux DRM event listをgpud経由で再確認し、通知channelのHANGUPと同じ
  wait graphで扱う。
- gpudでLinux DRM requestを検証・変換し、GEM、PRIME、syncobj、native fence、
  virtio-gpu command submission、KMS resource・page flipをsandboxへ渡す。
- PRIME exportのVMO viewはclientへ必要な`DUP | TRANSFER`とmapping権限だけを渡し、
  `REVOKE`等の管理権限はgpudに残す。
- LPRの`SCM_RIGHTS`でdma-bufを送る際は、VMO viewとgpudのPRIME leaseを一組で
  unixdへ渡す。受信時に種類・size・mapping権限・lease channelを検証し、失敗、破棄、
  closeの全経路で両capabilityを回収する。native fence FDも同じFD転送経路を通す。
- DRM fileはgpudのopen-file-description handleとして管理する。dup、fork、FD転送は
  lease参照を増やし、clientの強制終了は通知channelのHANGUPで回収する。最後の参照で
  sandbox側の実DRM fileをcloseする。
- sandbox強制終了時はgpudが旧DRM handle、mapping、queue、DMA/resource grantを失効・
  回収してからgenerationを進める。旧FDは失敗し、generation 2のsandboxへ再openできる。
  gpudはgeneration ownerとして生存し、強制終了の対象はsandbox processである。

## Mesa Gate

`lpr_mesa_multi_smoke`は独立したcard0 clientとrenderD128 clientを起動し、79x61の
linear BOを相互共有する。各clientはdma-bufを1回、native fenceを4回
`SCM_RIGHTS`転送し、4 frameについてown/peer双方の全画素を検証する。片方を
`SIGKILL`した後も、生存clientは共有BOを読み、追加描画とfence完了を行う。さらに
第3clientがrenderD128を再openし、描画・fence・全画素検証を行う。

restart Gateはgeneration 1のsandboxを強制終了し、旧FDの失効とgeneration 2への更新を
確認する。その後、generation 2で実Mesa cubeを描画し、D3D12 VirGL renderer、fence、
8x8全64画素の最終色、KMS page-flip eventを確認する。

## 最終確認

QEMU Gateは標準KVM・network有効構成、`--graphics virgl --display gtk`、host
`GALLIUM_DRIVER=d3d12`で実行した。`--no-kvm`と`--no-net`は使用していない。

| Gate | 結果 |
| --- | --- |
| `tests/run-lpr-qemu-drm-prime-smoke.sh` | dma-buf export/import、別process画素・表示: PASS |
| `tests/run-lpr-qemu-drm-card0-smoke.sh` | DRM ioctl、PRIME、syncobj、mmap、poll/read、dup/fork lease、client kill/reopen: PASS |
| `tests/run-lpr-qemu-mesa-multi-smoke.sh` | 3 client、dma-buf 2転送、native fence 8転送、全画素、SIGKILL回収、再open: PASS |
| `tests/run-lpr-qemu-drm-page-flip-smoke.sh` | 20 page flips、Mesa 8 frames、fence、checksum、最終全画素、KMS events: PASS |
| `tests/run-lpr-qemu-gpud-restart-smoke.sh` | sandbox kill、generation 1->2、旧FD失効、再open、Mesa再描画・fence・画素・表示: PASS |
| `kobox2.linux_mesa_virgl_recovery` | Linux版の2 client共有、pending fence、revoke、fresh generation、再描画・表示: PASS（42.09秒） |

restart後のrendererは`virgl (D3D12 (Intel(R) Graphics))`であり、llvmpipe、softpipe、
software rendererではない。最終画素は`rgba=0,255,0,255`、FNV-1a checksumは
`c07782b19c579525`だった。

host unitはgpud DRM file、GPU session、DRM translation、GPU query、LPR poll、service
ABI layout、pack Go testがPASSした。PachaOS kernel unitもPASSした。
`kobox2/linux-sandbox/linux`の差分は0で、変更は`kobox2/linux-sandbox/kobox`以下に限定した。

最終ログは`.artifacts/test-results/phase6-final/`と
`.artifacts/test-results/gpud-restart/`に保存した。
