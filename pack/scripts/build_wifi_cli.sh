#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Standalone static Linux ELF only; never installs files or rebuilds OSS.
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
compiler="${WIFI_MUSL_CC:-musl-gcc}"
prefix="${WIFI_OPENSSL_PREFIX:-$repo_root/.artifacts/wifi-build/openssl-prefix}"
sysroot="$repo_root/.artifacts/wifi-build/sysroot"
out="${WIFI_CLI_OUT:-$repo_root/.artifacts/wifi-cli}"
test -f "$prefix/lib/libcrypto.a"
test -f "$prefix/include/openssl/configuration.h"
mkdir -p "$out"
builtin=$("$compiler" -print-file-name=include)
"$compiler" -nostdinc -isystem "$sysroot/usr/include" -isystem "$builtin" \
    -I "$prefix/include" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -march=x86-64 -mtune=generic -mno-avx -mno-avx2 -mno-avx512f \
    -fno-pie -no-pie -static -ffunction-sections -fdata-sections \
    "$repo_root/userland/wifi/src/main.c" "$prefix/lib/libcrypto.a" \
    -pthread -ldl -Wl,--gc-sections,-z,noexecstack -o "$out/wifi"
"${OBJCOPY:-objcopy}" --strip-debug "$out/wifi"
chmod 755 "$out/wifi"
python3 - "$out/wifi" "$prefix/lib/libcrypto.a" "$repo_root/userland/wifi/src/main.c" <<'PY'
import hashlib, json, re, subprocess, sys
from pathlib import Path
elf, crypto, source = map(Path, sys.argv[1:])
headers = subprocess.check_output(['readelf', '-lW', str(elf)], text=True)
dynamic = subprocess.check_output(['readelf', '-dW', str(elf)], text=True)
assert 'INTERP' not in headers and 'NEEDED' not in dynamic, 'ELF must be static'
assembly = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(elf)], text=True)
for line in assembly.splitlines():
    m = re.match(r'\s*[0-9a-f]+:\s+([a-zA-Z0-9_.]+)\s*(.*)', line)
    if m and ((m[1].startswith('v') and m[1] not in ('verr', 'verw')) or
              re.search(r'%(?:ymm|zmm|k[0-7])\b', m[2])):
        raise RuntimeError('hosted CPU contract violation: ' + line)
record = {'elf': str(elf.resolve()), 'sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
          'libcrypto_sha256': hashlib.sha256(crypto.read_bytes()).hexdigest(),
          'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'static': True, 'avx_audit': 'PASS', 'backend_test': 'NOT_RUN', 'installed': False}
(elf.parent / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
print('WIFI_CLI_ELF=' + str(elf))
PY
