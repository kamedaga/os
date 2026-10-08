# PachaOS musl port

ここには musl の PachaOS native port に必要な glue を置く。

## Design

- static link を最初の対象にする。
- syscall ABI は PachaOS native ABI を直接使う。
- `libipc` と `libcapsule` は libc に混ぜず、daemon が明示的に link する library として分離する。
- filesystem syscall は filed を経由して sandbox の Linux VFS へ渡す。
- malloc は `mmap(MAP_ANONYMOUS)` を土台にする。
- thread/TLS は kernel の fd-based thread syscall と `thread_set_fs_base` を使う。

## Planned Files

```text
pachaos/
  include/       # PachaOS native ABI headers used by the port scaffold
  syscall/       # small OS syscall entry points not provided by musl target files
  smoke/         # first static smoke programs
  build/         # WSL clang build scripts
```

## ABI Boundary

musl port が直接使う kernel ABI は、process/thread、runtime、fd、mmap/munmap、futex に限定する。
daemon の IPC と capsule は `libipc` / `libcapsule` 経由で使う。
native `FUTEX_WAKE_WAIT` は wake と requeue の間へ挿入する。RDI=wake key、
RSI=wait key、R10=expected u32、R8=既存のtimeout ticks、RAX=wait status、
RDX=確定したwake数0/1（未実行・失敗はUINT64_MAX）。wait側の不一致・fault・
signalでも有効なoutbound wakeを一度試み、完了済みwakeを再送しない。
同一keyでは今回登録したcaller自身を除外する。後続番号は全shiftし、旧番号互換を
持たないため、loader／全native ELF／bootfs producerを同時に再buildする。
filesystem syscall の内部 IPC は `syscall/native.c` が raw VMO ABI を使う。
共有の `kobox2/protocol` codec と filed exec payload codec を link し、
旧 envelope / ring / 固定 payload の翻訳層は置かない。

file descriptor は実際の kernel descriptor を予約し、その番号と Linux
opaque handle の対応だけを可変長リストに保持する。予約は kernel の
FD_TABLE capacity と maximum に従って拡張する。read/stat/open cache と
独自 offset は保持せず、dup と read/write は Linux の struct file を使う。
path と I/O の上限は旧 native/filed の配列幅ではなく Linux または実際の
VMO / RAM / kernel descriptor policy に従う。

native exec は既存の spawn-and-exit 契約を維持する。argv/envp と継承
handle は共有の可変長 codec を使い、bootstrap VMO の handle 欄を filed
が patch する。成功後は親の handle だけを close し、子の duplicate を
残す。same-PID POSIX exec と MAP_SHARED の page-cache coherence は
この cutover では実装せず、後者は EOPNOTSUPP を返す。

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
PachaOS target bits and OS syscall glue. The syscall translation lives in
`musl/pachaos/syscall`, outside upstream; `build/runtime.mk` consumes the
upstream makefile unchanged and replaces only native port objects/includes.

`build/build-runtime.sh` builds the fuller PachaOS native musl runtime:

```text
.artifacts/musl-pachaos-runtime/install/usr/lib/crt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/crti.o
.artifacts/musl-pachaos-runtime/install/usr/lib/crtn.o
.artifacts/musl-pachaos-runtime/install/usr/lib/Scrt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/rcrt1.o
.artifacts/musl-pachaos-runtime/install/usr/lib/libc.a
.artifacts/musl-pachaos-runtime/install/usr/lib/libc.so
.artifacts/musl-pachaos-runtime/rootfs/usr/lib/pachaos/libc.so
.artifacts/musl-pachaos-runtime/rootfs/usr/lib/pachaos/ld-musl-x86_64.so.1
```

`build/build-app-dynamic.sh` links a PachaOS native dynamic executable against
that runtime without rebuilding libc for every server change. It uses the
installed crt objects and `libc.so`, emits PT_INTERP for
`/usr/lib/pachaos/ld-musl-x86_64.so.1`, and compiles only the app/server sources passed via
`PACHAOS_MUSL_APP_SOURCE` and `PACHAOS_MUSL_EXTRA_SOURCES`.

Deploy those runtime files only to `/usr/lib/pachaos`. The `/lib` interpreter
belongs to Linux programs run by LPR (including Alpine busybox) and must retain
the upstream Linux syscall ABI. Both native runtime files contain the same ELF;
the loader resolves its own `libc.so` dependency without searching Linux paths.
Additional native shared libraries require a native-only search policy: the
upstream loader's default non-libc search path still includes Linux `/lib` and
`/usr/lib`, and changing make's `syslibdir` does not change that search policy.

Bootstrap binaries loaded by hand-written early loaders should stay static
until that loader path explicitly supports PT_INTERP and `AT_BASE`. Services
started through filed native exec can move first.
