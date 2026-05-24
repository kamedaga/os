# Third Party Notices

CapabilityOS source code is licensed under the MIT License unless a file or
directory states otherwise. This document records third-party components that
are included in the repository or produced by the reproducible setup scripts.

This is not a license grant for third-party software. See each upstream project
for the full license text and copyright notices.

## Included in This Repository

| Component | Files | License | Source / notes |
|---|---|---|---|
| musl libc | `userland/fixtures/musl/libc.so`, `userland/fixtures/musl/ld-musl-x86_64.so.1` | MIT | https://musl.libc.org/ |
| dash | `userland/fixtures/shell/dash.elf` | BSD-3-Clause for the runtime binary | Built from dash 0.5.12. The upstream source tree also contains GPL-2.0+ build helper code in `mksignames.c`, which is not directly linked into dash. |
| FreeBSD TTY line discipline code | `userland/tty_service/src/bsd_line/` | BSD-2-Clause | Derived from FreeBSD files listed in `userland/tty_service/src/bsd_line/README.md`. |
| Cantarell fonts | `tools/Cantarell-Regular.ttf`, `tools/Cantarell-Bold.ttf` | SIL Open Font License 1.1 | https://gitlab.gnome.org/GNOME/cantarell-fonts |
| JetBrains Mono fonts | `tools/JetBrainsMono-Regular.ttf`, `tools/JetBrainsMono-Bold.ttf` | SIL Open Font License 1.1 | https://github.com/JetBrains/JetBrainsMono |

## Generated Runtime Artifacts

These files are not intended to be tracked in Git. The build scripts place them
under `.artifacts/userland-fixtures/` and `pactl` copies them into boot/rootfs
images as needed.

| Component | Generated files | License | Build script / source |
|---|---|---|---|
| apk-tools | `apk.elf` | GPL-2.0-only | `tools/build_wsl_apk_tools.sh`, https://gitlab.alpinelinux.org/alpine/apk-tools |
| Alpine package signing keys | `alpine-keys/*.rsa.pub` | See Alpine `alpine-keys` package | `tools/build_wsl_alpine_keys.sh`, https://gitlab.alpinelinux.org/alpine/alpine-keys |
| Alpine apk config | `apk-arch`, `apk-repositories`, `apk-world` | CapabilityOS config, MIT | `tools/build_wsl_apk_config.sh` |
| CA certificates bundle | `ca-certificates.crt` | See the host distribution's CA bundle notices | `tools/build_wsl_ca_certificates.sh` |
| curl | `curl.elf` | curl license | `tools/build_wsl_curl.sh`, https://curl.se/docs/copyright.html |
| GNU grep | `gnu-grep.elf` | GPL-3.0-or-later | `tools/build_wsl_gnu_grep.sh`, https://www.gnu.org/software/grep/ |
| Mbed TLS | `libmbedcrypto.so.16`, `libmbedtls.so.21`, `libmbedx509.so.7` | Apache-2.0 OR GPL-2.0-or-later | `tools/build_wsl_mbedtls.sh`, https://github.com/Mbed-TLS/mbedtls |
| zlib | `libz.so.1` | zlib license | `tools/build_wsl_zlib.sh`, https://www.zlib.net/zlib_license.html |
| Zstandard | `zstd.elf` | BSD OR GPL-2.0 | `tools/build_wsl_zstd.sh`, https://github.com/facebook/zstd |
| uutils coreutils | `uutils-coreutils*.elf`, `uutils-shim.elf` | MIT | `tools/build_wsl_uutils_*.sh`, https://github.com/uutils/coreutils |
| fastfetch | `fastfetch.elf` | MIT | `tools/build_wsl_fastfetch.sh`, https://github.com/fastfetch-cli/fastfetch |

The small musl-linked smoke/test binaries in `.artifacts/userland-fixtures/`
are built from local source under `userland/fixtures/src/wsl_musl/`.
