# PachaOS

> A hypervisor-first capability microkernel OS.
> Small kernel. Userland drivers. Dynamic DMA authority.

PachaOS は、OS 全体を capability machine として一貫させることを目指す実験的な OS です。  
kernel はできるだけ小さく保ち、driver、filesystem、compositor まで userland に押し出します。

## What Makes PachaOS Different

- **DMA / IOMMU を capability transfer の問題として扱う**
  - メモリ保護だけでなく DMA authority まで capability の rights に含めて扱います。
  - IOMMU を巨大な専用サブシステムとしてではなく、小さい kernel の中に最小構成で収めようとしています。

- **driver から compositor まで userland**
  - `virtio_input`、`virtio_gpu`、`virtio_blk` の driver 群だけでなく、VFS、persistent storage、window system まで user-space に寄せています。
  - kernel は mechanism を担当し、policy は `Init` と service 側へ移す方向です。

- **kernel が service 固有名を知りすぎない方向で進めている**
  - microkernel の見た目よりも、authority を capability と descriptor に還元できることを優先しています。
  - これは boot path や device bring-up にまで適用しようとしています。

- **hypervisor-first で現実の virtio world を相手にしている**
  - 対象は `x86_64 + UEFI + q35 + virtio` です。
  - 抽象論ではなく、virtio / DMA / UI を含んだ一式を capability model で揃えることを狙っています。

## What Already Works

- **capability ベースの kernel core**
  - page / endpoint / filesystem / VM object / exec image / untyped / queue capability 系が入っています。
  - `grant`、`send`、`share`、`revoke tree` の lineage を保つ方向で kernel test が揃っています。

- **DMA / IOMMU まわりの土台**
  - DMA mapping manager、device domain binding、queue capability が実装されています。
  - IOMMU no-cap-driver mode と DMA rights 同期のテストがあります。

- **user-space storage path**
  - `virtio_blk` と `persistent_fs` は現行 boot path に入っています。
  - bootfs だけで終わらない storage / rootfs 方向へ進める基礎ができています。

- **user-space UI stack**
  - compositor、GPU compositor、terminal、taskbar、input driver 群が codebase 上で揃っています。
  - window system も capability ベースで組まれています。

- **形式検証への足場**
  - `tla/` に DMA / capability 周辺のモデルがあります。
  - 将来の Capability model 検証は、完全なゼロスタートではありません。

## Core Ideas

- **Capability で DMA / IOMMU を動的に制御する**
  - メモリ権限だけでなく DMA authority も capability の rights として扱います。

- **最小 kernel、最大 userland**
  - `virtio_input`、`virtio_gpu`、`virtio_blk` の driver から UI まで user-space で構成します。

- **ハイパーバイザー向け microkernel**
  - 主な対象は `x86_64 + UEFI + q35 + virtio` です。

- **boot も capability handoff として扱う**
  - boot 後の system bring-up を、特別処理ではなく descriptor / capability の受け渡しとして整理します。

## Roadmap

- rootfs の完全対応
- kernel が init を知らないようにする
- TLA+ で Capability model を検証する

## Build / Run

```bash
cd kernel
zig build efi
cd ..
./setup.sh
./run.sh
# or ./run2.sh
```

## Technical Notes

実装寄りの説明は [TECHNICAL.md](TECHNICAL.md) に分離しています。
