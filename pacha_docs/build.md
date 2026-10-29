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

## Recipes and cache state

生成が必要な source は `pack/pack.yaml` の top-level `recipes` に定義する。
app は `file: {recipe: <id>, path: <output>}` で recipe の output を参照する。
同じ recipe を参照する複数 app は run、inputs、tools、outputs と cache state を共有するため、
どの app を選んだかによって rebuild 判定が変わることはない。

```yaml
recipes:
  example:
    run: ["bash", "pack/scripts/build_example.sh"]
    tools: ["bash", "clang", "ninja"]
    needs: ["generated_headers"]
    inputs:
      - {path: "userland/example", exclude: ["*.c", "*.h", "*.S"]}
      - "pack/scripts/build_example.sh"
    discover:
      - {kind: ninja, dir: ".artifacts/cmake/example"}
    outputs: [".artifacts/example/example.elf"]
apps:
  example:
    file: {recipe: example, path: ".artifacts/example/example.elf"}
```

全 recipe は flake の devShell 環境で実行する。pacgo は固定した最小環境から `nix develop`
の内外を比較し、devShell が追加・変更した変数と PATH の前置部分だけを
`.artifacts/pack/nix-env.json` に保存する。cache key は `flake.nix`、`flake.lock`、`nix/**`
と最小親環境。recipe 実行時は現在の環境へ保存した変数を重ね、保存した PATH 前置部分を
現在の PATH へ付ける。DISPLAY、PWD、TMPDIR など caller/session 固有値は cache しない。
そのため `./pacgo`（`nix develop` に入る）でも、host GPU の QEMU 用に host shell から
`PACGO_NIX_DEVELOP=1 ./pacgo` を起動しても、recipe の toolchain と判定は同じになる。
devShell が提供しない tool（`git`、`curl` など）は、その後ろの host PATH から解決される。
`tools` は recipe が実際に使う環境の PATH 上で解決された実体の内容、
`inputs` は展開された全 file の内容を追跡する。input は path 文字列のほか
`{path: ..., exclude: [...]}` を指定できる。`exclude` の basename glob と `**` path glob
に一致した file は宣言入力から除くが、この形式は `discover` がある recipe でしか使えない。
Makefile、Kbuild、Kconfig、JSON、linker script など除外対象外の file は引き続き宣言入力になる。

`discover` は build 成功後に build system が実際に読んだ dependency closure を state へ記録する。

- `kbuild`: O= directory 以下の `.*.cmd` と `include/config/auto.conf.cmd` を読み、source、header、Kconfig を記録する。relative path は各 submake の O= 側、次に srctree/VPATH 側で解決する。存在しない `$(wildcard ...)` だけを negative dependency とし、それ以外の欠落は discovery error にする。
- `kbuild-dir-file`: `dir` が指す1行の path fileを読み、その実行が選んだ digest 別 Kbuild directoryだけを `kbuild` と同様に探索する。path は `.artifacts` 内に限定する。
- `ninja`: recipe 環境の Ninja で default target の `-t inputs` と `-t deps` を取得し、source、header、CMake regeneration input を記録する。
- `depfile`: `glob` に一致する Makefile syntax の `.d` file を読む。escaped space と continuation に対応する。

recipe 自身の output と通常の `.artifacts` build tree は生成物として除外される。別 recipe の
output は実 dependency として保持され、直接の `needs` がなければ discovery は追加すべき
recipe 名を示して失敗する。workspace 外の header/toolchain file も記録し、`/nix/store`
は immutable path 自体、それ以外は内容 hash を追跡する。
`needs` にある recipe の output 配下の read は、その recipe の output digest
（下記）で追跡されるため、宣言入力に重ねて書く必要はない。

### kobox2 device package

各 device package は Linux 側と sandbox 側の 2 recipe に分かれている。

- Linux 側（`usb_hid_package`、`storage_package`、`block_packages`、`network_package`、
  `amdgpu_package`）は canonical kbuild、inventory、`build_boot_runtime.py`、
  `build_pci_package.py` を実行する。kbuild の no-op でも数十秒かかるため、userland の
  file は入力に持たない。depfile glob も `runtime/`、`provider/`、`manifest-build/`、
  `.deps/`、`*-manifest-generator.d` に限り、`sandbox.elf.d` を拾わない。
- sandbox 側（`usb_hid_sandbox`、`storage_sandbox`、`usb_storage_xhci_sandbox`、
  `nvme_sandbox`、`network_sandbox`、`amdgpu_sandbox`）は
  `pack/scripts/build_kobox2_sandbox.sh` で `build-sandbox.sh` を呼び、他 device の
  service symbol が無いことを確認する。source と header は `build-sandbox.sh` が書く
  `sandbox.elf.d` から discover する。

libpacha や adapter の変更では sandbox 側だけが再実行され、数秒で終わる。storage は
`storage_package` の `pci-package/` に sandbox を加えて bootfs 用の `package/` を作る。
AMDGPU は `amdgpu_sandbox`（`build_amdgpu_sandbox.sh`）が sandbox、`owner-policy.conf`
の digest、driver archive を作る。手動の `--cache-preflight` package は
`build_amdgpu_package.sh --cache-preflight` の後に `build_amdgpu_sandbox.sh --cache-preflight`
を実行する。

### Native ELF の build

musl libc と crt は build 設定ごとに一度だけ作る。`musl_libc`（既定）と `musl_libc_oz`
（`-Oz` と section GC 向け flag）の recipe が `musl/pachaos/build/build-libc.sh` で
`.artifacts/musl-pachaos/libc/{default,oz}` に sysroot、`libc.a`、crt object、`config`
（target と flag）、`libc.d`（読んだ musl source の depfile）を書く。約 270 の
translation unit は生成 Makefile と pacgo の jobserver で並列 compile し、内容が変わらなかった
sysroot file は以前の mtime を保つので、下流の不要な再 compile を起こさない。

native app は通常の CMake target である。各 `userland/<app>/CMakeLists.txt` は
`userland/cmake/pachaos-native.toolchain.cmake`（devShell の freestanding clang、
`x86_64-linux-musl`）を使い、`userland/cmake/pachaos_native.cmake` の
`pachaos_native_executable(<target> SOURCES ... [LIBC default|oz] ...)` で static PIE を
定義する。compile と link は上記 libc の sysroot に対して行い、host の CFLAGS/LDFLAGS や
build type 既定の flag は入らない。Ninja が translation unit 単位で差分 build するため、
共有 source の変更では各 app の該当 TU の compile と link だけが走り、libc の変更では
link だけが走る。LPR runtime（`userland/personality/linux/CMakeLists.txt`、
`build-lpr.sh` が install）も同じ toolchain と default libc の header で build する
shared object である。

共通 `pack/scripts/build_cmake_app.sh` は Ninja generator と native toolchain を固定し、
compiler、`ninja`、`ar`、`ranlib` を毎回 PATH 上の実体（recipe の tool state と同じ file）で
configure する。既存 cache が別 generator / toolchain の場合だけ `cmake --fresh` にする。
recipe の run 引数には project directory ではなく `<dir>/CMakeLists.txt` を渡す。run argv の
既存 path は暗黙の宣言入力になるため、directory を渡すと test や文書の変更でも再実行される。
compile される file は ninja discovery が記録する。

app の外にある source は独立した recipe とし、consumer は `needs` で参照する:

- `zydis_source`: `pack/scripts/download_zydis.sh` が pin した Zydis / Zycore を lock 下で
  一度だけ取得し、`.git` のない tree を `.artifacts/third_party/zydis-v4.1.1/tree` に置く。
- `live_console_assets`: libvterm の生成 table と console font を
  `.artifacts/third_party/libvterm-0.3.3/build` に組み立てる。
- `kobox2_protocol_check`: `kobox2/protocol/schema` から生成済み header が最新かを検査する。
  protocol の consumer は schema ではなく `arch`、`generated`、`include`、`src` を宣言し、
  この check を `needs` にする。
- `wsl_alpine_clang` と `copy_lpr_linux_musl`: LPR test 用の Alpine sysroot と musl runtime。
  smoke script は自分で取得せず、無ければ recipe 名を示して失敗する。

`tests/check_native_syscall_numbers.py` は native source 全体を走査するため、各 CMake build
の中ではなく `native_syscall_numbers` recipe として一度だけ実行し、CMake recipe はそれを
`needs` にする（stamp は常に同じ内容なので、再検査だけでは下流は再実行されない）。

`needs` に加え、別 recipe の output
配下を input にすると dependency edge が自動導出される。recipe state は各 `needs` の
output digest も記録する。依存 recipe が別の起動で再 build され output が変わった場合も、
consumer はその差分（`needed recipe output changed`）で再実行される。

選択した app の recipe と推移的な依存 recipe は DAG として実行される。独立した recipe
は並列実行され、全体の CPU 予算は `PACGO_BUILD_JOBS`（既定値は論理 CPU 数）で指定する。
pacgo の GNU make jobserver を子の make / Ninja / CMake build へ引き継ぐため、各スクリプト
が独自の並列数を追加する必要はない。jobserver に参加できない tool は、実行直前に利用可能な
token だけを予約し、終了時（異常終了と signal を含む）に返す。pacgo が渡すのは FIFO 形式で、
devShell の GNU make 4.4 と Ninja 1.13 はどちらもそのまま参加する。外側の make から fd 形式を
受け取った場合や古い Ninja では、`pacgo_ninja` / `pacgo_cmake_build` が token を予約して
`-j` を明示する。依存 recipe が実行されても
output 内容が同じなら、
それだけを理由に下流 recipe は再実行しない。

状態は `.artifacts/pack/build/recipes/<id>.json`、file hash の stat cache は
`.artifacts/pack/hashcache` に保存される。input/tool/run/needs output の変更、output の欠落や
外部変更に加え、discovered input の変更や negative dependency の出現があると recipe
全体を再実行する。`state status` は declared input、discovered input、negative dependency、
toolchain path のどれが変わったかを区別して表示する。

rebuild せず判定理由を確認する:

```bash
./pacgo state status
```

依存 tree を確認する:

```bash
./pacgo state graph
./pacgo state graph example
```

移行済みの既存 output が正しいと確認できている場合、その時点の状態を実行せず採用する:

```bash
./pacgo state adopt                 # output がすべて存在する recipe
./pacgo state adopt example other   # 指定 recipe（output 欠落時は error）
```

`state adopt` は「現在の output が inputs/tools に対して最新である」と利用者が明示的に
保証する操作であり、recipe 自体は実行しない。`discover` がある recipe は既存 build directory
を同じ parser で読み、その discovered/negative dependency も採用する。discover directory や
depfile がなければ、その recipe の adopt は拒否される。

recipe 宣言を実際の file access と照合する場合:

```bash
./pacgo audit deps example other
```

指定 recipe を jobserver と通常の recipe 環境のまま、Nix devShell の `strace` で一度実行する。
workspace 内の未宣言 read（directory 別、inputs 追加候補付き）、除外したのに discovery が
記録しなかった `excluded-but-read` file、読まれなかった宣言 input、
`tools` にない実行 binary、workspace 内の ENOENT（negative dependency 候補）を報告する。
未宣言 read、`excluded-but-read`、または未宣言 tool があれば non-zero。成功した trace run は通常 build と同様に
output と discovery state を記録し、失敗した run は state を記録しない。trace log は
`.artifacts/pack/logs/<recipe>.strace.log` に残る。通常 build は strace を使わない。

recipe の stdout と stderr は実行ごとに
`.artifacts/pack/logs/<recipe>.log` へ上書き保存される。失敗時は新しい recipe の開始を止め、
すでに実行中の recipe の終了を待ってから、失敗した各 log の末尾 60 行を表示する。

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

`--prepare` なしでは何もビルドしない。その代わり起動前に、既存イメージが
ソースと一致しているかをビルドせずに確認し、ずれていれば理由を警告する
（kernel の入力・stale なレシピ・manifest・rootfs・BOOTFS.IMG・Limine イメージ）。
kernel は最後の pacgo ビルドで zig キャッシュに記録された入力
（`.artifacts/pack/build/kernel-limine.json`）と比較する。
古いイメージで起動したくない場合は `--require-fresh` を付けると起動を拒否する。

build / sync も一緒にやってから起動:

```bash
./pacgo qemu --prepare
```

各段は変化がなければスキップされる。Limine イメージは入力
（KERNEL.ELF / INITAPP.ELF / BOOTFS.IMG / limine.conf / Limine 本体）と
イメージ自身のダイジェストを `limine-boot.img.inputs.json` に記録し、
すべて一致するときだけ再利用する。

再ビルドする段を絞る（rootfs / manifest / bootfs / Limine の反映は常に行う）:

```bash
./pacgo qemu --prepare --only kernel
./pacgo qemu --prepare --only userland:gpud_core,filed
./pacgo qemu --prepare --only kernel --only userland
```

`--only` で外した段が古い場合は起動前に警告する。

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

plain 表示では recipe ごとに `start <recipe> <secs>` と
`finish <recipe> <secs>` も出力する。

## Generated Files

`.artifacts/` 以下に置く。Git には含めない。

| Path | Role |
|---|---|
| `.artifacts/bin/pacgo` | compiled pacgo binary |
| `.artifacts/disk.img` | QEMU boot disk |
| `.artifacts/manifests/` | generated manifests |
| `.artifacts/userland/` | normalized userland artifacts |
| `.artifacts/pack/build/recipes/` | recipe ごとの build state |
| `.artifacts/pack/hashcache` | file content hash の persistent stat cache |
| `.artifacts/pack/logs/` | recipe ごとの stdout / stderr log |
| `.artifacts/pack/nix-env.json` | devShell 環境 cache |
