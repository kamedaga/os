# PachaOS musl port

ここには musl の PachaOS native port に必要な glue を置く。

## Design

- static link を最初の対象にする。
- syscall ABI は PachaOS native ABI を直接使う。
- `libipc` と `libcapsule` は libc に混ぜず、daemon が明示的に link する library として分離する。
- fs/rootfs がない段階では userland file server 前提の実装を入れない。
- malloc は `mmap(MAP_ANONYMOUS)` を土台にする。
- thread/TLS は kernel の fd-based thread syscall と `thread_set_fs_base` を使う。

## Planned Files

```text
pachaos/
  include/       # PachaOS native ABI headers used by the port scaffold
  syscall/       # small OS syscall entry points not provided by musl target files
  smoke/         # first static smoke programs
  build/         # WSL clang build scripts
  patches/       # small upstream patches if direct upstream edits are needed
```

## ABI Boundary

musl port が直接使う kernel ABI は、process/thread、runtime、fd、mmap/munmap、futex に限定する。
IPC と capsule は libc の中に隠さず、`libipc` / `libcapsule` 経由で使う。

## Current Scaffold

`build/build-smokes.sh` builds a small PachaOS musl sysroot and links a
static smoke ELF at:

```text
.artifacts/musl-pachaos/hello-libc-scaffold.elf
```

The sysroot contains upstream musl headers, PachaOS arch bits, generated
`bits/alltypes.h` / `bits/syscall.h`, `crt1.o` / `crti.o` / `crtn.o`, and
`libc.a`. Smoke programs are linked through the compiler's normal musl driver
path instead of `-nostdlib`; PachaOS-specific code is limited to the
`arch/pachaos` target files and OS syscall glue.

`build/build-runtime.sh` builds the fuller PachaOS native musl runtime:

```text
.artifacts/musl-pachaos-runtime/install/usr/lib/crt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/crti.o
.artifacts/musl-pachaos-runtime/install/usr/lib/crtn.o
.artifacts/musl-pachaos-runtime/install/usr/lib/Scrt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/rcrt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/libc.a
.artifacts/musl-pachaos-runtime/install/usr/lib/libc.so
.artifacts/musl-pachaos-runtime/rootfs/lib/libc.so
.artifacts/musl-pachaos-runtime/rootfs/lib/ld-musl-x86_64.so.1
```
