#!/usr/bin/env python3
"""Host-clock GUI observations; original guest applications, one disposable overlay.

Interactive use: python3 -i tests/qemu_xfce_gui_perf.py baseline-1
  vm.wallpaper(); vm.terminal(); vm.gtk(); vm.save('gtk-ready')
  vm.close()
Screenshots used for polling stay in /dev/shm. Only named checkpoints persist.
"""
import argparse
import atexit
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib

from qemu_xfce_apk_add_smoke import QMP

ROOT = Path(__file__).resolve().parents[1]


def type_text(qmp, text):
    special = {' ': 'spc', '-': 'minus', '_': 'shift-minus', '/': 'slash',
               ';': 'semicolon', '=': 'equal', '$': 'shift-4', '?': 'shift-slash',
               '>': 'shift-dot', '.': 'dot', ':': 'shift-semicolon', '+': 'shift-equal',
               '<': 'shift-comma', '|': 'shift-backslash', '&': 'shift-7',
               "'": 'apostrophe'}
    # Reject an unsupported command before injecting even its first character.
    for ch in text:
        if ch not in special and not (ch.isascii() and ch.isalnum()):
            raise ValueError(f'unsupported command character: {ch!r}')
    for ch in text:
        code = special.get(ch, ('shift-' + ch.lower()) if ch.isupper() else ch)
        qmp.chord('shift', code[6:]) if code.startswith('shift-') else qmp.key(code)
        # Leave time for inputd/Xorg/terminal to consume each report, even under
        # load. Command typing is outside the measured launch interval.
        time.sleep(.08)


class VM:
    def __init__(self, run, cpus=8, overlay=None, boot=None):
        self.out = ROOT/'.artifacts/gui-performance'/run
        self.out.mkdir(parents=True, exist_ok=False)
        self.tmp = tempfile.TemporaryDirectory(prefix='gui-perf-', dir='/dev/shm')
        self.overlay = Path(overlay).resolve() if overlay else self.out/'changes.qcow2'
        if overlay:
            info = json.loads(subprocess.check_output(
                ['qemu-img', 'info', '--output=json', str(self.overlay)]))
            assert info['format'] == 'qcow2'
            assert Path(info['full-backing-filename']).resolve() == ROOT/'.artifacts/disk.img'
        else:
            subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'raw',
                            '-b', str(ROOT/'.artifacts/disk.img'), str(self.overlay)], check=True)
        self.keep_overlay = False
        cmd = ['qemu-system-x86_64', '-machine', 'q35', '-enable-kvm', '-cpu', 'host',
               '-m', '4G', '-smp', str(cpus), '-S', '-no-reboot', '-display', 'none',
               '-monitor', 'none', '-serial', 'stdio', '-boot', 'order=c',
               '-device', 'intel-iommu,intremap=off,aw-bits=48',
               '-qmp', f'unix:{self.out}/qmp.sock,server=on,wait=off',
               '-drive', f'file={Path(boot).resolve() if boot else ROOT/".artifacts/limine-boot.img"},format=raw,if=ide,snapshot=on',
               '-drive', f'if=none,file={self.overlay},format=qcow2,id=rootdisk',
               '-device', 'nvme,drive=rootdisk,serial=capos-root',
               '-device', 'virtio-gpu-pci,disable-legacy=on,iommu_platform=on,id=pachagpu',
               '-device', 'virtio-keyboard-pci,disable-legacy=on,iommu_platform=on',
               '-device', 'virtio-tablet-pci,disable-legacy=on,iommu_platform=on',
               '-chardev', f'socket,id=virtcon0,path={self.out}/console.sock,server=on,wait=off',
               '-device', 'virtio-serial-pci,disable-legacy=on,iommu_platform=on,id=virtserial0',
               '-device', 'virtconsole,chardev=virtcon0,name=org.pachaos.console.0',
               '-net', 'none', '-netdev', 'user,id=net0', '-device',
               'virtio-net-pci,netdev=net0,disable-legacy=on,iommu_platform=on,csum=off,gso=off,'
               'guest_csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,'
               'host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,mrg_rxbuf=off']
        (self.out/'command.json').write_text(json.dumps(cmd, indent=2))
        self.started = time.monotonic()
        self.rows = []
        self.capture_costs = []
        self.fault = threading.Event()
        self.closed = False
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.qmp = None
        atexit.register(self.close)
        deadline = time.monotonic() + 10
        while not (self.out/'console.sock').exists():
            if self.proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('QEMU did not open console')
            time.sleep(.02)
        self.qmp = QMP(str(self.out/'qmp.sock'))
        self.qmp.sock.settimeout(20)
        self.console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.console.connect(str(self.out/'console.sock'))
        for stream, name in [(self.proc.stdout, 'serial'), (self.proc.stderr, 'stderr'),
                             (self.console.makefile('rb'), 'console')]:
            threading.Thread(target=self.drain, args=(stream, name), daemon=True).start()
        self.qmp.execute('cont')

    def drain(self, stream, name):
        with (self.out/(name+'.log')).open('wb') as log:
            for line in stream:
                elapsed = time.monotonic() - self.started
                log.write(f'{elapsed:.6f} '.encode() + line)
                log.flush()
                if re.search(rb'PAGE FAULT|GENERAL PROTECTION|USER fault|KERNEL PANIC', line):
                    self.fault.set()
                    print('GUEST FAULT', name, elapsed, flush=True)

    def capture(self):
        before = time.monotonic()
        path = Path(self.tmp.name)/'screen.ppm'
        self.qmp.execute('screendump', {'filename': str(path), 'format': 'ppm', 'device': 'pachagpu'})
        after = time.monotonic()
        magic, dimensions, maximum, pixels = path.read_bytes().split(b'\n', 3)
        assert magic == b'P6' and maximum == b'255'
        w, h = map(int, dimensions.split())
        assert len(pixels) == w*h*3
        self.capture_costs.append(after-before)
        return before, after, w, h, pixels

    @staticmethod
    def samples(frame, rect, step=8):
        _, _, w, h, pixels = frame
        x0, y0, x1, y1 = rect
        return [pixels[(y*w+x)*3:(y*w+x)*3+3]
                for y in range(y0, min(y1,h), step) for x in range(x0, min(x1,w), step)]

    def save(self, name, frame=None):
        frame = frame or self.capture()
        _, _, w, h, pixels = frame
        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
        raw = b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
        png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,8,2,0,0,0))
        png += chunk(b'IDAT', zlib.compress(raw, 3))+chunk(b'IEND', b'')
        (self.out/(name+'.png')).write_bytes(png)

    def until(self, label, start, predicate, timeout=90, interval=.05):
        last_negative = start
        while time.monotonic()-start < timeout:
            frame = self.capture()
            if self.fault.is_set():
                self.save(label+'-fault', frame)
                raise RuntimeError('guest fault')
            if predicate(frame):
                row = dict(event=label, lower_s=max(0,last_negative-start),
                           upper_s=frame[1]-start, qmp_capture_s=frame[1]-frame[0],
                           start_host_s=start-self.started,
                           paint_lower_host_s=last_negative-self.started,
                           paint_upper_host_s=frame[1]-self.started)
                self.rows.append(row)
                self.save(label, frame)
                self.report()
                print(row, flush=True)
                return row
            last_negative = frame[0]
            time.sleep(interval)
        self.save(label+'-timeout')
        raise TimeoutError(label)

    def wallpaper(self):
        return self.until('wallpaper', self.started,
            lambda f: sum(b > r+20 and g > r+20 for r,g,b in
                self.samples(f,(256,192,768,576),16)) > 600, interval=.5)

    def terminal(self, label='terminal'):
        start = time.monotonic()
        self.qmp.chord('ctrl','alt','t')
        def ready(f):
            black = sum(max(rgb)<25 for rgb in self.samples(f,(20,120,470,330)))
            ink = sum(min(rgb)>180 for rgb in self.samples(f,(5,82,170,100),1))
            return black > 1300 and ink > 100
        return self.until(label, start, ready)

    def shell(self, command):
        type_text(self.qmp, command)
        start = time.monotonic()
        self.qmp.key('ret')
        return start

    def profile_dump_done(self, app, timeout=60):
        """Do not overlap the next launch with the previous exit-time dump."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            logs = '\n'.join(p.read_text() for p in
                (self.out/'serial.log', self.out/'console.log') if p.exists())
            starts = [(float(m[1]), int(m[2])) for m in re.finditer(
                rf'^([\d.]+) \[gui-prof\] begin (\d+) \d+ {int(app)} ', logs, re.M)]
            if starts:
                pid = max(starts)[1]
                if re.search(rf'\[gui-prof\] done {pid}\s*$', logs, re.M):
                    return pid
            if self.fault.is_set():
                raise RuntimeError('guest fault during profile dump')
            time.sleep(.1)
        raise TimeoutError(f'profile dump for app {app}')

    def terminal_direct(self, label='terminal-direct'):
        # Stock option prevents DBus forwarding to the control terminal. The
        # fixed position lets us detect the new prompt, not the existing one.
        def ready(f):
            black = sum(max(rgb)<25 for rgb in self.samples(f,(700,400,950,650)))
            # Exclude both the empty terminal's initial block cursor at x=305
            # and the prompt's trailing cursor at x=395. Require prompt text.
            ink = sum(min(rgb)>180 for rgb in self.samples(f,(320,307,380,323),1))
            return black > 900 and ink > 60
        assert not ready(self.capture()), 'target terminal region already ready'
        start = self.shell('xfce4-terminal --disable-server --geometry=80x24+300+250 > /dev/hvc0 2>&1 &')
        return self.until(label,start,ready)

    def gtk(self, extra='', label='gtk-first-window', counters_elf=None):
        command = f'gtk4-demo {extra} > /dev/hvc0 2>&1 &'
        if counters_elf is None:
            start = self.shell(command)
        else:
            type_text(self.qmp, command)
            self.kernel_counters(label+'-before', counters_elf)
            start = time.monotonic()
            self.qmp.key('ret')
        row = self.until(label, start,
            lambda f: sum(min(rgb)>190 for rgb in self.samples(f,(100,120,900,650)))>1600)
        if counters_elf is not None:
            self.kernel_counters(label+'-after', counters_elf)
        return row

    @staticmethod
    def crop(frame, rect):
        _, _, w, h, pixels = frame
        x0,y0,x1,y1 = rect
        return b''.join(pixels[(y*w+x0)*3:(y*w+min(x1,w))*3]
                        for y in range(y0,min(y1,h)))

    def click(self, x, y):
        frame = self.capture()
        events = [{'type':'abs','data':{'axis':'x','value':int(x*32767/(frame[2]-1))}},
                  {'type':'abs','data':{'axis':'y','value':int(y*32767/(frame[3]-1))}}]
        self.qmp.execute('input-send-event', {'events':events})
        self.qmp.execute('input-send-event', {'events':[
            {'type':'btn','data':{'button':'left','down':True}}]})
        # A tablet report contains final button state. Down/up in one QMP
        # batch can produce no click. Allow the pressed report to be consumed;
        # measure release-to-paint for release-activated GTK buttons.
        time.sleep(.08)
        start = time.monotonic()
        self.qmp.execute('input-send-event', {'events':[
            {'type':'btn','data':{'button':'left','down':False}}]})
        return start

    def response_click(self, label, x, y, rect):
        before = self.crop(self.capture(),rect)
        start = self.click(x,y)
        return self.until(label,start,
            lambda f: sum(a!=b for a,b in zip(before,self.crop(f,rect)))>300,
            timeout=10, interval=.005)

    def search_key_pair(self, label):
        """GTK demo search field must be open, focused, and initially empty."""
        def glyph(frame):
            return sum(max(rgb) < 80 for rgb in
                       self.samples(frame, (276,230,284,246), 1)) > 8
        assert not glyph(self.capture()), 'search field is not empty'
        start = time.monotonic()
        self.qmp.key('k')
        self.until(label+'-type',start,glyph,timeout=10,interval=.005)
        time.sleep(.4)
        assert glyph(self.capture()), 'search glyph disappeared unexpectedly'
        start = time.monotonic()
        self.qmp.key('backspace')
        self.until(label+'-erase',start,lambda f:not glyph(f),timeout=10,interval=.005)
        time.sleep(.4)

    def move_fishbowl_into_view(self):
        """Move the focused stock-size demo window fully onto the test screen."""
        self.qmp.chord('alt','f7')
        time.sleep(.2)
        self.qmp.execute('input-send-event', {'events': [
            {'type':'abs','data':{'axis':'x','value':int(240*32767/1023)}},
            {'type':'abs','data':{'axis':'y','value':int(280*32767/767)}}]})
        self.qmp.key('ret')
        time.sleep(2)

    def changes(self, label, rect, seconds=10, interval=.01):
        """Displayed-content changes, not application FPS; sampling can alias."""
        start = time.monotonic()
        frames = []
        while time.monotonic()-start < seconds:
            f = self.capture()
            frames.append([f[0]-start, f[1]-start, zlib.crc32(self.crop(f,rect))])
            if self.fault.is_set(): raise RuntimeError('guest fault')
            time.sleep(interval)
        (self.out/(label+'-samples.json')).write_text(json.dumps(frames))
        distinct = sum(a[2]!=b[2] for a,b in zip(frames,frames[1:]))
        row = dict(event=label, samples=len(frames), changes=distinct,
                   seconds=frames[-1][1]-frames[0][1],
                   displayed_changes_per_s=distinct/(frames[-1][1]-frames[0][1]))
        self.rows.append(row)
        self.save(label)
        self.report()
        print(row,flush=True)
        return row

    def report(self):
        (self.out/'results.json').write_text(json.dumps({'events': self.rows,
            'capture_count':len(self.capture_costs),
            'max_capture_s':max(self.capture_costs, default=0)}, indent=2))

    def kernel_counters(self, label, elf):
        """Non-stopping diagnostic snapshot; rows are not globally atomic."""
        log = (self.out/'serial.log').read_text()
        bases = {k:int(v,16) for k,v in re.findall(
            r'limine: (Physical|Virtual) base:\s+(0x[0-9a-f]+)',log)}
        symbols = subprocess.check_output(['nm','-n',str(elf)], text=True)
        snapshot = dict(host_s=time.monotonic()-self.started)
        # KVM's versioned pvclock conversion supplies the guest TSC rate
        # without guest clock quantization or a prior run's UART calibration.
        pv = re.search(r'^([0-9a-f]+) b realtime_clock\.pv_time$', symbols, re.M)
        if pv:
            physical = int(pv[1],16)-bases['Virtual']+bases['Physical']
            samples = []
            for index in range(2):
                path = self.out/(label+f'-pvclock-{index}.bin')
                self.qmp.execute('pmemsave',dict(val=physical,size=32,filename=str(path)))
                samples.append(path.read_bytes())
            if samples[0] == samples[1]:
                version, _, stamp, ns, mult, shift, flags = struct.unpack('<IIQQIbB2x',samples[0])
                if not version & 1 and mult and -63 <= shift <= 63:
                    snapshot['tsc_hz'] = 1e9*(2**32)/(mult*(2.0**shift))
                    snapshot['tsc_source'] = 'stable KVM pvclock snapshot'
        for name in ('smp_perf_counters','munmap_perf_counters','vmo_perf_counters'):
            if not re.search(r' B '+name+r'$',symbols,re.M):
                if name == 'vmo_perf_counters': continue  # Older diagnostic ELF.
                raise ValueError(f'missing diagnostic symbol: {name}')
            address = int(re.search(r'^([0-9a-f]+) B '+name+r'$',symbols,re.M)[1],16)
            physical = address-bases['Virtual']+bases['Physical']
            path = self.out/(label+'-'+name+'.bin')
            self.qmp.execute('pmemsave',dict(val=physical,size=64*16*8,filename=str(path)))
            words = struct.unpack('<1024Q',path.read_bytes())
            snapshot[name] = [sum(words[cpu*16+i] for cpu in range(64)) for i in range(16)]
        (self.out/(label+'-counters.json')).write_text(json.dumps(snapshot,indent=2))
        print(snapshot,flush=True)
        return snapshot

    def close(self):
        if self.closed: return
        self.closed = True
        if self.qmp:
            try: self.qmp.execute('quit')
            except (OSError, EOFError): pass
            self.qmp.close()
        if self.proc.poll() is None: self.proc.terminate()
        self.proc.wait(timeout=10)
        self.report()
        self.tmp.cleanup()
        if not self.keep_overlay:
            self.overlay.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('run')
    parser.add_argument('--cpus', type=int, default=8)
    args = parser.parse_args()
    vm = VM(args.run, args.cpus)
    if not __import__('sys').flags.interactive:
        try:
            vm.wallpaper()
            vm.terminal()
            vm.gtk()
            time.sleep(2)
            vm.save('gtk-settled')
        finally: vm.close()
