# Build and Boot

WSL Linux 上で動作確認を行っている。

## 構成

| Path | Role |
|---|---|
| `pacgo` | Nix devShell 内で動く Go 製 runner |
| `pack/` | pacgo 本体・CLI・pack 定義 |
| `pack/pack.yaml` | kernel / apps / rootfs / startup / disk の宣言的定義 |
| `nix/` / `flake.nix` | WSL Linux 用 toolchain 環境 |
| `.artifacts/` | 生成物・disk image・manifest・pacgo state |

## Requirements

```bash
nix develop
```

これで toolchain が全部入る。以降のコマンドはこの shell の中で叩く。

## Plan

今の workspace と app 状態を確認する。

```bash
./pacgo plan
```

## Build

userland の生成物を更新（既存 rootfs は変更しない）:

```bash
./pacgo build userland --no-rootfs
```

rootfs の再生成も行う場合（下記「Sync」の注意を確認）:

```bash
./pacgo build userland
```

特定 app だけ:

```bash
./pacgo build userland exec --no-rootfs
./pacgo build userland fastfetch --no-rootfs
```

kernel をビルド:

```bash
./pacgo build kernel
```

通常の Limine 起動用出力: `kernel/zig-out/bin/limine/pacha-kernel.elf`

## Manifests

```bash
./pacgo gen manifests
```

| Manifest | Path |
|---|---|
| bootfs | `.artifacts/manifests/bootfs.generated.txt` |
| rootfs | `.artifacts/manifests/rootfs.generated.txt` |
| startup | `.artifacts/manifests/startup.generated.txt` |

## Sync

rootfs を再生成して反映:

注意: 現在の ext4 実装は差分同期ではない。新しく生成した ext4 を
rootfs パーティション全体へ書き込むため、manifest にないゲスト内の変更や
ファイルは保持されない。必要なデータを退避してから実行する。
`build userland`（`--no-rootfs` なし）と `qemu --prepare` にも同じ注意が必要。

```bash
./pacgo sync rootfs
```

bootfs / ESP を更新:

```bash
./pacgo sync bootfs
```

## QEMU

起動だけ:

```bash
./pacgo qemu
```

build / sync も一緒にやってから起動:

```bash
./pacgo qemu --prepare
```

console を別 terminal に分ける:

```bash
./pacgo qemu --new-terminal
```

通常の Limine 起動ログ: `.artifacts/qemu-limine.log`

### Xfce / D3D12 (WSLg)

配備済みの通常 boot profile は `/sbin/pacha-boot-session` から
`startx /root/.xinitrc` → `dbus-run-session -- startxfce4` を実行する。
`/etc/pacha_boot_profile` が `console-shell` の場合は自動起動しない。

```bash
PACGO_NIX_DEVELOP=1 CAPOS_QEMU=/usr/bin/qemu-system-x86_64 \
GALLIUM_DRIVER=d3d12 GDK_BACKEND=x11 MESA_D3D12_DEFAULT_ADAPTER_NAME=Intel \
./pacgo qemu --memory 8G --cpus 4 --graphics virgl --display gtk,gl=on \
  --input-profile keyboard-tablet \
  --qemu-arg=-device --qemu-arg=ramfb --qemu-arg=-vga --qemu-arg=none
```

このコマンドは既存ディスクを再生成しない。2026-10-21 の更新後の構成では
4G で `OutOfFreePages` が発生したため、確認には 8G を使用した。
GPU acceleration の判定は Xorg log の
`glamor X acceleration enabled on virgl (D3D12 (` を確認し、
`LLVMPIPE` を成功として扱わない。

## Tests

boot smoke:

```bash
./pacgo test smoke
```

fd IPC だけを最小 init で確認:

```bash
./pacgo test fd-ipc-smoke
```

TTY interaction test:

```bash
./pacgo qemu-test
```

任意の文字列を送る場合:

```bash
./pacgo qemu-test --send "/bin/fastfetch" --expect "PachaOS"
```

## Progress

CI やログ保存したい場合は plain 表示:

```bash
PACGO_PROGRESS=plain ./pacgo sync rootfs
```

## Generated Files

`.artifacts/` 以下に置く。Git には含めない。

| Path | Role |
|---|---|
| `.artifacts/bin/pacgo` | compiled pacgo binary |
| `.artifacts/disk.img` | QEMU boot disk |
| `.artifacts/manifests/` | generated manifests |
| `.artifacts/userland/` | normalized userland artifacts |
