# Compositor virtio-gpu / GPU Compositor 実装メモ

## 目的

従来の framebuffer 直書き compositor に加えて、ユーザー空間 `virtio-gpu` ドライバ経由で表示する GPU compositor を追加した。

構成は次のとおり。

```text
classic compositor
  -> framebuffer 直書き

gpu compositor
  -> virtgpu driver (user space)
  -> virtqueue
  -> virtio-gpu device
```

## 実装した役割

ユーザー空間 `virtgpu` ドライバは、最小限として次の 4 つを担当する。

1. virtqueue 管理
2. GPU resource 作成
3. framebuffer backing の attach と transfer
4. scanout 設定と flush

公開 API は次の 5 つ。

```zig
virtgpu_init()
virtgpu_create_fb(width, height)
virtgpu_set_scanout(resource)
virtgpu_transfer(resource, rect)
virtgpu_flush(resource)
```

この API は最初は scanout 用 framebuffer 1 枚向けに入れたが、その後複数 resource を持てるように拡張した。

## 変更点

### 1. kernel 側

- `kernel/src/virtio_probe.zig`
  - modern `virtio-gpu` PCI device を probe する `probeGpuModern()` を追加。
- `kernel/src/main.zig`
  - Process1 用に compositor GPU config page を追加。
  - probe 結果の `common/notify/isr/device` MMIO 情報を config page に公開。
  - Process1 に必要な MMIO capability を install。
  - deferred compositor launch で `classic` / `gpu` を切り替えられるようにした。
  - `COMPOS.ELF` と `GPUCOMP.ELF` の両方をロードする。

- `kernel/src/capability.zig`
  - fresh page 用の fast path `mapFreshUserPage()` を追加。

- `syscall_alloc_map_pages`
  - backing page など大量 page 確保向けに高速化。
  - map 後に cap table から drop するフラグを追加。

### 2. user space 側

- `kernel/user_programs/virtgpu.zig`
  - compositor から使う専用の最小 `virtio-gpu` ドライバを新規追加。
  - control queue を初期化。
  - `RESOURCE_CREATE_2D`
  - `RESOURCE_ATTACH_BACKING`
  - `SET_SCANOUT`
  - `TRANSFER_TO_HOST_2D`
  - `RESOURCE_FLUSH`
  - を同期実行する。
  - scanout 用だけでなく複数 resource を持てるようにした。
  - `virtgpu_flush_rect()` を追加し、dirty rect 単位 flush を可能にした。

- `kernel/user_programs/compositor_core.zig`
  - compositor 本体を共通化。
  - `classic` と `gpu` の両モードから共通の window 管理と入力処理を使う。
  - GPU モードでは primary scanout backing に直接描く。
  - dirty rect のみ `TRANSFER_TO_HOST_2D` / `FLUSH` する。
  - window ごとに source shadow を持ち、差分検出して dirty rect を計算する。
  - window ごとに独立 `virtio-gpu resource` を持ち、window source 更新時にその resource にも dirty upload する。

- `kernel/user_programs/compositor.zig`
  - classic compositor 用 wrapper。

- `kernel/user_programs/gpu_compositor.zig`
  - GPU compositor 用 wrapper。

### 3. 実行環境

- `run.sh`
  - `virtio-gpu` を出すため `virtio-vga` を追加。
  - 既定 VGA は `-vga none` で無効化。

## 実装の段階整理

### 段階1: 最小 DMA 経路

- scanout 用 1 resource
- `back_buffer -> gpu backing -> transfer -> flush`
- userspace `virtgpu` driver の bring-up

### 段階2: GPU compositor 分離

- `COMPOS.ELF` と `GPUCOMP.ELF` を分離
- boot 時に `F=classic`, `G=gpu`, `Enter=launch`
- GPU compositor が `first present` まで成功

### 段階3: 現在

- primary scanout backing に dirty rect だけ直接描画
- GPU モードでは `back_buffer` を常用しない
- window ごとの `virtio-gpu resource`
- window source の shadow 比較による dirty rect 検出
- dirty rect 単位 `transfer + flush`
- cursor queue による hardware cursor plane
- GPU モードでは cursor を primary scene に焼き込まず、`UPDATE_CURSOR` / `MOVE_CURSOR` で別 plane 更新

## 現状の制約

- window resource 自体を GPU で scene 合成しているわけではない。
  - scene 合成そのものはまだ CPU。
  - GPU がやっているのは backing memory を scanout へ出す部分。
- `RESOURCE_UNREF` は未実装。
- scanout は `0` 固定。
- cursor image は固定 64x64 / hot spot `(0, 0)`。
- title/meta の継続監視はまだ弱い。
- capability レベルの DMA オブジェクト化までは行っておらず、`virtio-gpu` backing transfer を DMA 経路として使っている。

## 次の改善候補

1. window resource を将来的な GPU scene 合成へ接続する
2. `GET_DISPLAY_INFO` を読んで scanout 情報を動的取得する
3. `RESOURCE_UNREF` を入れて resource の寿命管理をする
4. mouse cursor 専用 plane / queue を検討する
5. virgl や 3D path を使う場合は、その段階で本当の GPU scene 合成へ進む
