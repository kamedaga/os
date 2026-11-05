#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=.artifacts/epiphany-emfile/klogudp-build
payload=.artifacts/epiphany-emfile/klogudp-payload
mkdir -p "$build/bridge/pacha" "$payload/cmd"
cmake -S userland/klogudp -B "$build" -G Ninja
cmake --build "$build"
# Only adapt internal native wrappers for the Linux launch bridge. Original
# sources stay untouched; generated copies are retained for review. This
# bridges existing ABIs and does not add a Linux syscall or kernel ABI.
python3 - "$build" <<'PY'
from pathlib import Path
import sys
b=Path(sys.argv[1])
for src,dst in [('userland/libpacha/src/syscall.c',b/'bridge/syscall.c'),
 ('userland/libpacha/include/pacha/native_syscall_retry.h',b/'bridge/pacha/native_syscall_retry.h'),
 ('userland/libpacha/include/pacha/poll.h',b/'bridge/pacha/poll.h'),
 ('userland/libpacha/include/pacha/abi.h',b/'bridge/pacha/abi.h')]:
    s=Path(src).read_text()
    s=s.replace('__asm__ volatile("syscall"','__asm__ volatile("call *klogudp_gate(%%rip)"')
    dst.write_text('extern void *klogudp_gate;\n'+s)
PY
musl-gcc -static -O2 -std=c11 -Wall -Wextra -Werror -mno-red-zone \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I"$build/bridge" -Iuserland/libpacha/include -Iuserland/libipc/include \
  -Iuserland/filed/include -Iuserland/libfiled_client/include \
  -Ikobox2/protocol/include -Ikobox2/protocol/generated/include \
  userland/klogudp/src/launch.c "$build/bridge/syscall.c" \
  userland/libipc/src/ipc.c userland/libfiled_client/src/client.c \
  userland/filed/src/exec_payload.c kobox2/protocol/src/filesystem.c \
  -o "$build/klogudp-start"
install -m 0755 "$build/klogudp.elf" "$payload/cmd/klogudp.elf"
install -m 0755 "$build/klogudp-start" "$payload/cmd/klogudp-start"
python3 - "$payload" <<'PY'
from pathlib import Path
import hashlib,sys
p=Path(sys.argv[1])
files=sorted(x for x in p.rglob('*') if x.is_file() and x.name!='FILES.tsv')
(p/'FILES.tsv').write_text('path\tmode\tsha256\n'+''.join(f'{x.relative_to(p)}\t0755\t{hashlib.sha256(x.read_bytes()).hexdigest()}\n' for x in files))
PY
