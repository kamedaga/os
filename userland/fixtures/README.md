# Userland Fixtures

This directory contains small tracked runtime inputs that are intentionally
included in the public repository.

## Layout

| Directory | Contents |
|---|---|
| `base/` | Small CapabilityOS-owned config and smoke script files. |
| `musl/` | Included musl runtime loader/libc images. |
| `shell/` | Included dash shell runtime image. |
| `src/wsl_musl/` | Local C sources for smoke and helper binaries generated into `.artifacts/`. |

Large or license-sensitive generated runtime artifacts are not tracked here.
They are generated under `.artifacts/userland-fixtures/` by the `tools/build_*`
scripts and copied into rootfs by `pactl`.
