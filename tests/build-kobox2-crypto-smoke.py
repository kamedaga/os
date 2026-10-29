#!/usr/bin/env python3
"""Build only our smoke ELF, using the already-built baseline static libcrypto."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def output(*command):
    return subprocess.check_output(command, text=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl-prefix', type=Path,
        default=ROOT / '.artifacts/wifi-build/openssl-prefix')
    parser.add_argument('--sysroot', type=Path,
        default=ROOT / '.artifacts/wifi-build/sysroot')
    parser.add_argument('--output-dir', type=Path,
        default=ROOT / '.artifacts/tests/kobox2-crypto-smoke')
    parser.add_argument('--cc', default=os.environ.get('WIFI_MUSL_CC', 'musl-gcc'))
    parser.add_argument('--run-host', action='store_true',
        help='also run the ELF on host Linux; this does not prove the PachaOS FP contract')
    args = parser.parse_args()
    prefix, sysroot, destination = (path.resolve() for path in
        (args.openssl_prefix, args.sysroot, args.output_dir))
    library = prefix / 'lib/libcrypto.a'
    config = prefix / 'include/openssl/configuration.h'
    if not library.is_file() or not config.is_file() or not (sysroot / 'usr/include').is_dir():
        parser.error('build the existing baseline OpenSSL/sysroot first; this script never builds OSS')
    if '#  define OPENSSL_NO_ASM' not in config.read_text():
        parser.error('expected the baseline generic OpenSSL prefix with OPENSSL_NO_ASM')
    cc = shlex.split(args.cc)
    builtin = output(*cc, '-print-file-name=include').strip()
    sources = [ROOT / 'tests/kobox2_crypto_smoke.c', ROOT / 'tests/kobox2_crypto_signal.S']
    destination.mkdir(parents=True, exist_ok=True)
    elf = destination / 'crypto-smoke.elf'
    command = [*cc, '-nostdinc', '-isystem', str(sysroot / 'usr/include'),
        '-isystem', builtin, '-I', str(prefix / 'include'), '-std=c11', '-O2', '-g',
        '-Wall', '-Wextra', '-Werror', '-march=x86-64', '-mtune=generic',
        '-mno-avx', '-mno-avx2', '-mno-avx512f', '-fno-pie', '-no-pie',
        '-static', '-ffunction-sections', '-fdata-sections',
        *(str(path) for path in sources), str(library), '-pthread', '-ldl',
        '-Wl,--gc-sections,-z,noexecstack', '-o', str(elf)]
    subprocess.run(command, check=True)
    elf.chmod(0o755)
    headers = output('readelf', '-lW', str(elf))
    dynamic = output('readelf', '-dW', str(elf))
    if 'INTERP' in headers or 'NEEDED' in dynamic:
        raise RuntimeError('smoke must be a self-contained static Linux ELF')
    disassembly = output('objdump', '-d', '--no-show-raw-insn', str(elf))
    for line in disassembly.splitlines():
        match = re.match(r'\s*[0-9a-f]+:\s+([a-zA-Z0-9_.]+)\s*(.*)', line)
        if match and ((match[1].startswith('v') and match[1] not in ('verr', 'verw')) or
                re.search(r'%(?:ymm|zmm|k[0-7])\b', match[2])):
            raise RuntimeError('instruction exceeds hosted FXSAVE512 contract: ' + line)
    record = {'elf': str(elf), 'sha256': digest(elf),
        'compiler': output(*cc, '--version').splitlines()[0], 'command': command,
        'libcrypto': str(library), 'libcrypto_sha256': digest(library),
        'openssl_configuration_sha256': digest(config),
        'source_sha256': {str(path.relative_to(ROOT)): digest(path) for path in sources},
        'static': True, 'avx_audit': 'PASS', 'success_marker': 'CRYPTO_SMOKE=PASS',
        'host_run': False, 'pachaos_run': 'NOT_RUN'}
    if args.run_host:
        run = subprocess.run([str(elf)], check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (destination / 'host.log').write_text(run.stdout)
        print(run.stdout, end='')
        if run.returncode or 'CRYPTO_SMOKE=PASS' not in run.stdout:
            raise RuntimeError(f'host smoke failed: exit={run.returncode}')
        record['host_run'] = True
    (destination / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print('CRYPTO_SMOKE_ELF=' + str(elf))


if __name__ == '__main__':
    main()
