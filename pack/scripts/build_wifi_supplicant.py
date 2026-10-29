#!/usr/bin/env python3
"""Compile an unmodified upstream supplicant with pinned static dependencies."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / '.artifacts/wifi-build'
THIRD = ROOT / '.artifacts/third_party/wpa_supplicant'
VERSION = '2.12'
SOURCE_SHA256 = '08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6'
MIRROR = 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/'


def fetch(url, path, expected):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_suffix(path.suffix + '.download')
        urllib.request.urlretrieve(url, temporary)
        assert hashlib.sha256(temporary.read_bytes()).hexdigest() == expected, url
        temporary.replace(path)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, str(path)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def audit_cpu_contract(path):
    # Include VEX instructions using XMM only, not just YMM/ZMM operands.
    instructions = subprocess.check_output(
        ['objdump', '-d', '--no-show-raw-insn', str(path)], text=True)
    for line in instructions.splitlines():
        match = re.match(r'\s*[0-9a-f]+:\s+([a-zA-Z0-9_.]+)\s*(.*)', line)
        if match and ((match[1].startswith('v') and match[1] not in ('verr', 'verw'))
                or re.search(r'%(?:ymm|zmm|k[0-7])\b', match[2])):
            raise RuntimeError(f'{path.name}: instruction exceeds hosted SSE contract: {line}')


def build_openssl(cc):
    """Match the hosted CPU's FP/SSE contract, including all linked crypto."""
    third = ROOT / '.artifacts/third_party/openssl'
    archive = third / 'openssl-3.5.9.tar.gz'
    fetch('https://www.openssl.org/source/' + archive.name, archive,
        '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a')
    source = third / 'openssl-3.5.9'
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(third, filter='data')
    build = WORK / 'openssl-build'
    prefix = WORK / 'openssl-prefix'
    build.mkdir(exist_ok=True)
    args = ['perl', str(source / 'Configure'), 'linux-generic64',
        'no-shared', 'no-module', 'no-tests', 'no-asm',
        '--prefix=' + str(prefix), '--libdir=lib', 'CC=' + cc,
        'CFLAGS=-O2 -march=x86-64 -mtune=generic -mno-avx -mno-avx2 '
        '-mno-avx512f -DOPENSSL_AES_CONST_TIME']
    fingerprint = hashlib.sha256(json.dumps(args).encode()).hexdigest()
    stamp = build / 'configure.sha256'
    if not stamp.exists() or stamp.read_text() != fingerprint:
        run(*args, cwd=build)
        stamp.write_text(fingerprint)
    run('make', '-j' + os.environ.get('PACGO_BUILD_JOBS', '4'), 'build_sw', cwd=build)
    run('make', 'install_sw', cwd=build)
    return prefix


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    archive = THIRD / f'wpa_supplicant-{VERSION}.tar.gz'
    fetch(f'https://w1.fi/releases/{archive.name}', archive, SOURCE_SHA256)
    source = THIRD / f'wpa_supplicant-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(THIRD, filter='data')
    sysroot = WORK / 'sysroot'
    packages = json.loads((ROOT / 'pack/wifi/wpa-build-dependencies.json').read_text())
    for item in packages:
        name = f'{item["name"]}-{item["version"]}.apk'
        package_path = WORK / 'apks' / name
        fetch(MIRROR + name, package_path, item['sha256'])
        with tarfile.open(package_path) as package:
            # These are compiler inputs only. Never install APKs into rootfs.
            members = [m for m in package.getmembers() if m.name.startswith('usr/')]
            package.extractall(sysroot, members=members, filter='data')
    compiler = os.environ.get('WIFI_MUSL_CC', 'musl-gcc')
    builtin_headers = subprocess.check_output(
        [compiler, '-print-file-name=include'], text=True).strip()
    cc = f'{compiler} -nostdinc -isystem {sysroot}/usr/include -isystem {builtin_headers}'
    openssl = build_openssl(cc)
    cc += f' -I{openssl}/include -march=x86-64 -mtune=generic -mno-avx -mno-avx2 -mno-avx512f'
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(sysroot),
        PKG_CONFIG_LIBDIR=str(sysroot / 'usr/lib/pkgconfig'))
    # Make does not track compiler/flag changes. Never reuse objects built
    # for another crypto implementation or CPU contract merely by mtime.
    object_identity = hashlib.sha256(json.dumps([cc, env['PKG_CONFIG_SYSROOT_DIR'],
        (ROOT / 'pack/wifi/wpa_supplicant.config').read_text(),
        str(openssl)]).encode()).hexdigest()
    object_stamp = WORK / 'wpa-objects.sha256'
    if not object_stamp.exists() or object_stamp.read_text() != object_identity:
        shutil.rmtree(WORK / 'obj', ignore_errors=True)
        object_stamp.write_text(object_identity)
    run('make', '-j' + os.environ.get('PACGO_BUILD_JOBS', '4'),
        '-C', str(source / 'wpa_supplicant'),
        'CONFIG_FILE=' + str(ROOT / 'pack/wifi/wpa_supplicant.config'),
        'BUILDDIR=' + str(WORK / 'obj'), 'CC=' + cc,
        f'LDFLAGS=-static -L{openssl}/lib -L{sysroot}/usr/lib -pthread',
        'wpa_supplicant', 'wpa_cli', env=env)
    output = ROOT / '.artifacts/kobox2-wifi-production/userspace'
    output.mkdir(parents=True, exist_ok=True)
    licenses = output / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(source / 'COPYING', licenses / 'wpa_supplicant.txt')
    shutil.copyfile(ROOT / '.artifacts/third_party/openssl/openssl-3.5.9/LICENSE.txt',
        licenses / 'openssl.txt')
    for name in ('wpa_supplicant', 'wpa_cli'):
        path = output / name
        shutil.copyfile(source / 'wpa_supplicant' / name, path)
        run('objcopy', '--strip-debug', str(path))
        path.chmod(0o755)
        headers = subprocess.check_output(['readelf', '-l', str(path)], text=True)
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        assert 'INTERP' not in headers and 'NEEDED' not in dynamic, name
        audit_cpu_contract(path)
    run(str(output / 'wpa_supplicant'), '-v')
    (output / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256((output / name).read_bytes()).hexdigest() + '  ' + name + '\n'
        for name in ('wpa_supplicant', 'wpa_cli')))


if __name__ == '__main__':
    main()
