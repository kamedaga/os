# Virtual Framebuffer Capability Step1

## 目的
- アプリプロセスとコンポジタープロセスが同一の「仮想Framebuffer領域」を共有できる土台を作る。
- 物理Framebufferへの直接描画とは別に、仮想Framebuffer専用の capability を導入する。

## 変更内容
- `kernel/src/kernel.zig`
  - `VirtualFramebufferCapability` を追加。
    - `paddr`, `size_bytes`, `width`, `height`, `pixels_per_scan_line`, `pixel_format`
    - `allow_read`, `allow_write`
  - `KernelState` に `virtual_framebuffer_caps` を追加。
  - API を追加。
    - `grantVirtualFramebufferCap(to, cap)`
    - `getVirtualFramebufferCap(principal)`
    - `canAccessVirtualFramebuffer(principal, paddr, size_bytes, writable)`
  - unit test を追加。
    - Process0/Process1 共有アクセス可否
    - read-only cap での write 拒否

- `kernel/src/main.zig`
  - 仮想Framebuffer共有ページを 1 page (`4096B`) で確保（Process0 所有）。
  - Process1 へ page capability を `grantCap` で付与。
  - Process0/Process1 双方へ `grantVirtualFramebufferCap` を設定。
  - `canAccessVirtualFramebuffer` で検証後、同一 paddr を以下 VA に map。
    - Process0: `0x2000_B000`
    - Process1: `0x2000_B000`
  - ブートログ追加:
    - `virtual framebuffer capability ready`
    - `vfb_paddr`, `app_va`, `compositor_va`

## 現時点の範囲
- capability モデル導入と共有マップの準備まで。
- compositor の描画経路はまだ物理Framebuffer直書き中心で、仮想Framebuffer合成本体は次段で置換。

## 次ステップ
1. Process0 アプリを `virtual_framebuffer_app_va` 描画へ切替。
2. Compositor を `virtual_framebuffer_compositor_va` 入力で合成する構成へ変更。
3. dirty-rect で仮想FB更新領域のみを物理FBへ反映する。
