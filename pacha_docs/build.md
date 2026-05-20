# Build and Boot

PachaOS の公開リポジトリを clone した状態から、ビルド、ディスク作成、QEMU 起動、`apk add` まで確認する手順です。

## Host

確認した環境は次の組み合わせです。

| Layer | Version |
|---|---|
| Windows Zig | 0.15.2 |
| Windows Python | 3.14.0 |
| Rust stable | rustc 1.95.0 / cargo 1.95.0 |
| WSL | Linux 6.6.114.1-microsoft-standard-WSL2 |
| QEMU | 8.2.2 |
| WSL Python | 3.12.3 |
| WSL gcc | 13.3.0 |
| WSL clang | 18.1.3 |
| WSL cmake | 3.28.3 |
| WSL pkg-config | 1.8.1 |

Windows 側の Rust に default toolchain がない場合は、`cargo +stable` を明示するか、先に次を実行します。

```powershell
rustup default stable
```

WSL 側には少なくとも次が必要です。

```bash
sudo apt update
sudo apt install qemu-system-x86 ovmf build-essential musl-tools cmake pkg-config wget curl python3
```

`ninja` はこの確認環境では未導入でした。現在の通常ビルドでは必須ではありません。

## Line Endings

WSL の `bash` で実行する `*.sh` は LF である必要があります。リポジトリの `.gitattributes` は `*.sh` を LF に固定しています。

clone 済みの作業ツリーで CRLF が残っている場合は、いったん checkout し直してください。

```powershell
git add --renormalize .
```

## Build

初回、または公開リポジトリから完全に再現したい場合は `setup full` を使います。これは kernel、userland、manifest、disk image をまとめて生成します。

```powershell
cargo +stable run --manifest-path tools/pactl/Cargo.toml -- setup full
```

default Rust toolchain を設定済みなら、次でも同じです。

```powershell
.\pactl.cmd setup full
```

生成物は `.artifacts/` に置かれます。Git には含めません。

主な出力は次です。

| Path | Role |
|---|---|
| `.artifacts/disk.img` | QEMU boot disk |
| `.artifacts/manifests/bootfs.generated.txt` | bootfs manifest |
| `.artifacts/manifests/rootfs.generated.txt` | rootfs manifest |
| `.artifacts/manifests/startup.generated.txt` | startup manifest |
| `.artifacts/userland-fixtures/` | generated userland runtime files |

## Runtime Inputs

公開リポジトリにそのまま入れている runtime input は次です。

| Component | Version | Source in tree |
|---|---:|---|
| musl libc | 1.2.4 | `userland/fixtures/musl/` |
| dash | 0.5.12 | `userland/fixtures/shell/dash.elf` |

次の runtime input は build script が取得または生成し、`.artifacts/userland-fixtures/` に置きます。

| Component | Version / source | Build script |
|---|---|---|
| apk-tools | `apk-tools-static-2.14.9-r3.apk` from Alpine v3.22 | `tools/build_wsl_apk_tools.sh` |
| Alpine repositories | Alpine v3.22 main/community | `tools/build_wsl_apk_config.sh` |
| Alpine package keys | `alpine-keys-2.5-r0.apk` from Alpine v3.22 | `tools/build_wsl_alpine_keys.sh` |
| curl | 8.19.0 | `tools/build_wsl_curl.sh` |
| Mbed TLS | 3.6.6 | `tools/build_wsl_mbedtls.sh` |
| zlib | 1.3.2 | `tools/build_wsl_zlib.sh` |
| Zstandard | 1.5.7 | `tools/build_wsl_zstd.sh` |
| uutils coreutils | 0.4.0 | `tools/build_wsl_uutils_*.sh` |
| fastfetch | 2.61.0 | `tools/build_wsl_fastfetch.sh` |
| CA certificates | WSL host CA bundle | `tools/build_wsl_ca_certificates.sh` |

License information is summarized in `THIRD_PARTY_NOTICES.md`.

## Boot and apk Smoke

`setup full` 後に QEMU で boot して `apk add zlib` を確認します。

```powershell
wsl -e bash -lc "cd /mnt/c/Users/kamer/Documents/os && python3 tools/apk_add_smoke.py --timeout 120 --package zlib --out .artifacts/apk-add-smoke"
```

成功すると、OS 内で Alpine v3.22 の index を取得し、temporary root に `musl` と `zlib` を install したあと、`zlib` が表示されます。

確認時の結果:

```text
(1/2) Installing musl (1.2.5-r12)
(2/2) Installing zlib (1.3.2-r0)
OK: 0 MiB in 2 packages
zlib
__CAPABILITYOS_SMOKE_DONE_38196__:0
```

`apk update` 単体の smoke も次で確認できます。

```powershell
wsl -e bash -lc "cd /mnt/c/Users/kamer/Documents/os && python3 tools/apk_update_smoke.py --timeout 90 --out .artifacts/apk-update-smoke --command 'apk update'"
```

## Kernel-only Check

kernel だけを確認する場合は `kernel/` を作業ディレクトリにします。

```powershell
cd kernel
zig build efi
```

repo root で `zig build-obj` を直接実行しないでください。root に object file を撒きやすいためです。
