#!/usr/bin/env python3
"""Host-only protocol/TTY checks with an explicit test build and fake backend."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import signal
import subprocess
import tempfile
import termios
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WifiCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = ROOT / '.artifacts/tests/wifi-cli-host'
        out.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix='fixture-', dir=out)
        cls.root = Path(cls.temporary.name)
        cls.runtime = cls.root / 'run/pachaos/wifi'
        cls.config_dir = cls.root / 'etc/pachaos'
        cls.runtime.mkdir(parents=True, mode=0o700)
        cls.config_dir.mkdir(parents=True, mode=0o700)
        cls.binary = cls.root / 'wifi-test'
        cc = os.environ.get('WIFI_MUSL_CC', 'musl-gcc')
        sysroot = ROOT / '.artifacts/wifi-build/sysroot'
        openssl = ROOT / '.artifacts/wifi-build/openssl-prefix'
        builtin = subprocess.check_output([cc, '-print-file-name=include'], text=True).strip()
        subprocess.run([cc, '-nostdinc', '-isystem', str(sysroot / 'usr/include'),
            '-isystem', builtin, '-I', str(openssl / 'include'), '-std=c11', '-O2',
            '-Wall', '-Wextra', '-Werror', '-static', '-march=x86-64', '-mno-avx',
            '-mno-avx2', '-mno-avx512f', '-DWIFI_TEST_ROOT=' + json.dumps(str(cls.root)),
            '-DWIFI_TEST_OWNER_UID=' + str(os.geteuid()),
            str(ROOT / 'userland/wifi/src/main.c'), str(openssl / 'lib/libcrypto.a'),
            '-pthread', '-ldl', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        for directory in (self.runtime, self.config_dir):
            directory.chmod(0o700)
            for path in directory.iterdir():
                path.unlink()
        self.backend_errors = []
        self.write(self.runtime / 'session', '0123456789abcdef\n')

    def write(self, path, text):
        temporary = path.with_name('.backend-' + path.name)
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(text)
        temporary.replace(path)

    def run_cli(self, *args):
        return subprocess.run([str(self.binary), *args], capture_output=True, timeout=3)

    def backend(self, command, status=0, stale_first=False):
        def serve():
            try:
                deadline = time.monotonic() + 2
                while not (self.runtime / 'request').exists():
                    if time.monotonic() >= deadline:
                        raise AssertionError('CLI never published request')
                    time.sleep(0.005)
                request = (self.runtime / 'request').read_text()
                match = re.fullmatch(r'([0-9a-f]{32}) (scan|connect|disconnect)\n', request)
                assert match and match[2] == command
                assert match[1].startswith('0123456789abcdef')
                assert (self.runtime / 'request').stat().st_mode & 0o777 == 0o600
                if stale_first:
                    self.write(self.runtime / 'response', 'f' * 32 + ' 0\nSTALE\n')
                    time.sleep(0.06)
                if command == 'scan':
                    self.write(self.runtime / 'scan-results', 'bssid / frequency / signal / flags / ssid\nfixture\n')
                self.write(self.runtime / 'response', f'{match[1]} {status}\nbackend fixture reply\n')
            except Exception as error:
                self.backend_errors.append(error)
        thread = threading.Thread(target=serve)
        thread.start()
        return thread

    def join_backend(self, thread):
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        if self.backend_errors:
            raise self.backend_errors[0]

    def terminal_cli(self, args, password=None, interrupt=None, eof=False):
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        process = subprocess.Popen([str(self.binary), *args], stdin=slave,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 2
            while termios.tcgetattr(slave)[3] & termios.ECHO:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError('password input never disabled echo')
                time.sleep(0.005)
            if interrupt:
                process.send_signal(interrupt)
            elif eof:
                os.write(master, b'\x04')
            else:
                os.write(master, password + b'\n')
            stdout, stderr = process.communicate(timeout=3)
            self.assertEqual(termios.tcgetattr(slave)[3], original[3], 'TTY flags not restored')
            # A one-byte SAE fixture can occur in ordinary words such as
            # "fixture"; use full passphrases for the no-secret-output check.
            if password and len(password) >= 8:
                self.assertNotIn(password, stdout + stderr)
            return process.returncode, stdout, stderr
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    def test_backend_absent_never_success(self):
        result = self.run_cli('disconnect')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'no success confirmed', result.stderr)
        self.assertEqual(result.stdout, b'')

    def test_session_required_and_mode_checked(self):
        self.write(self.runtime / 'status', 'old cached status\n')
        (self.runtime / 'session').unlink()
        result = self.run_cli('status')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'')
        self.assertFalse((self.runtime / 'request').exists())
        self.write(self.runtime / 'session', 'invalid\n')
        self.assertNotEqual(self.run_cli('scan').returncode, 0)
        self.write(self.runtime / 'session', '0123456789abcdef\n')
        (self.runtime / 'session').chmod(0o644)
        self.assertNotEqual(self.run_cli('scan').returncode, 0)

    def test_session_change_rejects_old_success(self):
        process = subprocess.Popen([str(self.binary), 'disconnect'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 2
            while not (self.runtime / 'request').exists():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.005)
            request_id = (self.runtime / 'request').read_text().split()[0]
            self.write(self.runtime / 'session', 'fedcba9876543210\n')
            self.write(self.runtime / 'response', request_id + ' 0\nOLD_SUCCESS\n')
            stdout, _ = process.communicate(timeout=3)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(stdout, b'')
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_stale_response_scan_and_backend_failure(self):
        thread = self.backend('scan', stale_first=True)
        result = self.run_cli('scan')
        self.join_backend(thread)
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'fixture', result.stdout)
        self.assertNotIn(b'STALE', result.stdout)
        (self.runtime / 'request').unlink()
        thread = self.backend('disconnect', status=-19)
        result = self.run_cli('disconnect')
        self.join_backend(thread)
        self.assertNotEqual(result.returncode, 0)

    def test_status_permissions_and_symlink(self):
        self.write(self.runtime / 'status', 'backend=not-ready\n')
        result = self.run_cli('status')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'backend=not-ready\n')
        self.assertFalse((self.runtime / 'request').exists())
        (self.runtime / 'status').chmod(0o644)
        self.assertNotEqual(self.run_cli('status').returncode, 0)
        (self.runtime / 'status').unlink()
        (self.runtime / 'status').symlink_to('/etc/passwd')
        self.assertNotEqual(self.run_cli('status').returncode, 0)

    def test_status_heartbeat_stale_or_future_rejected(self):
        path = self.runtime / 'status'
        self.write(path, 'stale status must not print\n')
        for stamp in (time.time() - 6, time.time() + 30):
            os.utime(path, (stamp, stamp))
            result = self.run_cli('status')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_lock_deadline(self):
        self.write(self.runtime / 'lock', '')
        with (self.runtime / 'lock').open('r+') as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_cli('disconnect')
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.runtime / 'request').exists())

    def test_lock_held_until_matching_response(self):
        process = subprocess.Popen([str(self.binary), 'disconnect'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 2
            while not (self.runtime / 'request').exists():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.005)
            with (self.runtime / 'lock').open('r+') as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                request_id = (self.runtime / 'request').read_text().split()[0]
                self.write(self.runtime / 'response', request_id + ' 0\n')
                process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0)
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_transition_config_roundtrip_and_disconnect_preserves_it(self):
        password = b'a"b\\c#d\\xe'
        ssid = 'test"ssid#'
        thread = self.backend('connect')
        code, stdout, _ = self.terminal_cli(['connect', ssid, '--country', 'jp'], password)
        self.join_backend(thread)
        self.assertEqual(code, 0)
        self.assertIn(b'backend fixture reply', stdout)
        path = self.config_dir / 'wifi.conf'
        text = path.read_text()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertIn('ssid=' + ssid.encode().hex() + '\n', text)
        self.assertIn('psk=' + hashlib.pbkdf2_hmac('sha1', password, ssid.encode(), 4096, 32).hex() + '\n', text)
        escaped = password.decode().replace('\\', '\\\\').replace('"', '\\"')
        self.assertIn('sae_password=P"' + escaped + '"\n', text)
        self.assertIn('key_mgmt=WPA-PSK SAE\n', text)
        self.assertIn('country=JP\n', text)
        self.assertIn('update_config=0\n', text)
        self.assertFalse(list(self.config_dir.glob('*.tmp')))
        original = path.read_bytes()
        (self.runtime / 'request').unlink()
        thread = self.backend('disconnect')
        self.assertEqual(self.run_cli('disconnect').returncode, 0)
        self.join_backend(thread)
        self.assertEqual(path.read_bytes(), original)

    def test_open_and_wpa3_config(self):
        thread = self.backend('connect')
        result = self.run_cli('connect', 'open-ssid', '--open')
        self.join_backend(thread)
        self.assertEqual(result.returncode, 0)
        text = (self.config_dir / 'wifi.conf').read_text()
        self.assertIn('key_mgmt=NONE\n', text)
        self.assertNotIn('password', text)
        self.assertNotIn('psk=', text)
        (self.runtime / 'request').unlink()
        thread = self.backend('connect')
        code, _, _ = self.terminal_cli(['connect', 'sae', '--wpa3'], b'x')
        self.join_backend(thread)
        self.assertEqual(code, 0)
        text = (self.config_dir / 'wifi.conf').read_text()
        self.assertIn('key_mgmt=SAE\n', text)
        self.assertIn('ieee80211w=2\n', text)
        self.assertNotIn('psk=', text)

    def test_cancel_eof_and_overlong_restore_echo(self):
        for action in ('signal', 'eof', 'overlong'):
            with self.subTest(action=action):
                code, _, _ = self.terminal_cli(['connect', 'ssid'],
                    password=b'x' * 64 if action == 'overlong' else None,
                    interrupt=signal.SIGTERM if action == 'signal' else None,
                    eof=action == 'eof')
                self.assertNotEqual(code, 0)
                self.assertFalse((self.config_dir / 'wifi.conf').exists())
                self.assertFalse((self.runtime / 'request').exists())

    def test_reject_unsafe_config_and_cli_arguments(self):
        target = self.root / 'untouched'
        target.write_text('unchanged')
        (self.config_dir / 'wifi.conf').symlink_to(target)
        self.assertNotEqual(self.run_cli('connect', 'ssid', '--open').returncode, 0)
        self.assertEqual(target.read_text(), 'unchanged')
        self.assertFalse((self.runtime / 'request').exists())
        self.assertNotEqual(self.run_cli('connect', 'ssid', '--open', '--wpa2').returncode, 0)
        self.assertNotEqual(self.run_cli('connect').returncode, 0)
        self.runtime.chmod(0o755)
        self.assertNotEqual(self.run_cli('scan').returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
