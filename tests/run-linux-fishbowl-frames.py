#!/usr/bin/env python3
"""Standard Linux DRM comparison; shared Pacha disk is read-only + RAM overlay."""
import json
import base64
import gzip
import hashlib
import os
from pathlib import Path
import runpy
import socket
import subprocess
import sys
import threading
import time

from qemu_xfce_apk_add_smoke import QMP

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
trace_ioctl = os.environ.get('TRACE_XORG_IOCTL') == '1'
measure_kvm = os.environ.get('INTERACTION_KVM_STATS') == '1'
unsynced = os.environ.get('LINUX_FISHBOWL_UNSYNCED') == '1'
clock_trial = os.environ.get('LINUX_GTK_CLOCK_TRIAL') == '1'
unlimited = os.environ.get('GDK_FRAME_CLOCK_UNLIMITED') == '1'
if unlimited:
    assert clock_trial and unsynced, 'Unlimited clock requires the isolated, unsynced trial'
command = [
    'qemu-system-x86_64', '-machine', 'q35', '-cpu', 'host', '-enable-kvm',
    '-m', '2G', '-smp', '4', '-monitor', 'none', '-no-reboot',
    '-cdrom', str(root/'.artifacts/m3.6b-linux-baseline/alpine-virt-3.22.5-x86_64.iso'),
    '-boot', 'order=d', '-vga', 'none', '-display', 'gtk,gl=on',
    '-device', 'virtio-gpu-gl-pci,id=pachagpu,xres=640,yres=480,disable-legacy=on',
    '-device', 'virtio-keyboard-pci', '-device', 'virtio-tablet-pci',
    '-drive', f'if=none,file={root}/.artifacts/disk.img,format=raw,readonly=on,id=pacharoot',
    '-device', 'nvme,drive=pacharoot,serial=pacha-baseline',
    '-drive', f'file=fat:ro:{root}/tests/fixtures,format=raw,if=virtio,readonly=on',
    '-netdev', 'user,id=net0', '-device', 'virtio-net-pci,netdev=net0',
    '-serial', f'unix:{out}/console.sock,server=on,wait=on',
    '-qmp', f'unix:{out}/qmp.sock,server=on,wait=off',
    '-trace', f'enable=virtio_gpu_*,file={out}/gpu-trace.log',
    '-msg', 'timestamp=on',
]
if clock_trial:
    trial_dir = root/'.artifacts/gtk3-frameclock-trial/export'
    assert (trial_dir/'libgdk-3.so.0').is_file()
    trial_sha = hashlib.sha256((trial_dir/'libgdk-3.so.0').read_bytes()).hexdigest()
    (out/'clock-trial.json').write_text(json.dumps(dict(
        unlimited=unlimited, sha256=trial_sha, duration_ms=10000), indent=2))
    command += ['-drive', f'file=fat:ro:{trial_dir},format=raw,if=virtio,readonly=on']
(out/'command.json').write_text(json.dumps(command, indent=2))
transcript = bytearray()
trace_anchors = {}
with (out/'qemu.log').open('wb') as log:
    process = subprocess.Popen(command, stdout=log, stderr=log)
    console = socket.socket(socket.AF_UNIX)
    qmp = None
    try:
        deadline = time.monotonic()+20
        while not (out/'console.sock').exists():
            assert process.poll() is None and time.monotonic() < deadline
            time.sleep(.1)
        console.connect(str(out/'console.sock'))

        def drain():
            with (out/'console.log').open('wb') as stream:
                while data := console.recv(65536):
                    transcript.extend(data)
                    stream.write(data)
                    stream.flush()
                    for marker in ('IOCTL_ACTIVE', 'IOCTL_INACTIVE'):
                        if marker not in trace_anchors and (marker+'\r\n').encode() in transcript:
                            trace_anchors[marker] = time.time()
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()

        def wait(marker, timeout=60):
            deadline = time.monotonic()+timeout
            while marker not in transcript:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f'waiting for {marker!r}; see console.log')
                time.sleep(.1)

        def send(line):
            console.sendall(line.encode()+b'\n')

        def cpu_snapshot():
            rows = {}
            for path in Path(f'/proc/{process.pid}/task').glob('*/stat'):
                data = path.read_text()
                fields = data[data.rfind(')')+2:].split()
                rows[path.parent.name] = dict(
                    name=data[data.find('(')+1:data.rfind(')')],
                    ticks=int(fields[11])+int(fields[12]))
            return dict(time=time.monotonic(), hz=os.sysconf('SC_CLK_TCK'), rows=rows)

        def kvm_snapshot(label):
            for target in ('vm', 'vcpu'):
                stats = qmp.execute('query-stats',
                    {'target':target, 'providers':[{'provider':'kvm'}]})
                (out/f'{label}-kvm-{target}.json').write_text(json.dumps(stats, indent=2))

        wait(b'localhost login:')
        send('root')
        wait(b'localhost:~#')
        setup_env = 'FISHBOWL_UNSYNCED=1 ' if unsynced else ''
        if clock_trial:
            setup_env += 'GTK_FRAME_CLOCK_TRIAL=1 '
        send('mkdir /baseline; mount -o ro /dev/vda1 /baseline; '
             + setup_env + 'sh /baseline/linux_virgl_fishbowl.sh')
        wait(b'LINUX_XFCE_STARTED')
        if clock_trial:
            assert (trial_sha + '  /linux/tmp/gtk-frameclock/libgdk-3.so.0').encode() in transcript
        qmp = QMP(str(out/'qmp.sock'))
        assert qmp.execute('query-kvm')['enabled']
        (out/'cpus.json').write_text(json.dumps(qmp.execute('query-cpus-fast')))
        time.sleep(20)
        if trace_ioctl:
            send('mkdir -p /sys/kernel/tracing; mount -t tracefs tracefs /sys/kernel/tracing; '
                 'T=/sys/kernel/tracing; echo 0 > $T/tracing_on; echo mono > $T/trace_clock; '
                 'echo 8192 > $T/buffer_size_kb; echo > $T/trace; '
                 'for E in sys_enter_ioctl sys_exit_ioctl; do '
                 'echo "common_pid == $(pidof Xorg)" > $T/events/syscalls/$E/filter; '
                 'echo 1 > $T/events/syscalls/$E/enable; done; echo IOCTL_TRACE_READY')
            wait(b'\r\nIOCTL_TRACE_READY\r\n')
        io_env = 'vblank_mode=0 ' if unsynced else (
            'FRAME_OBSERVER_ALL_IO=1 LD_PRELOAD=/usr/lib/frame-observer.so ')
        if clock_trial:
            io_env += ('LD_LIBRARY_PATH=/tmp/gtk-frameclock '
                       f'GDK_FRAME_CLOCK_UNLIMITED={int(unlimited)} ')
        duration_ms = 10000 if clock_trial else 30000
        send('chroot /linux /usr/bin/env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/0 '
             'FRAME_OBSERVER_FIXED_COUNT=1 FRAME_OBSERVER_FIXED_BUTTON=1 '
             f'FRAME_OBSERVER_DURATION_MS={duration_ms} '
             'GTK_MODULES=/usr/lib/frame-observer.so '
             + io_env +
             'gtk3-demo --run=fishbowl >/linux/tmp/fishbowl.log 2>&1 &')
        time.sleep(12)

        def move(x, y):
            qmp.execute('input-send-event', {'events': [
                {'type':'abs', 'data':{'axis':'x', 'value':round(x/639*32767)}},
                {'type':'abs', 'data':{'axis':'y', 'value':round(y/479*32767)}}]})
        move(266,333)
        time.sleep(.3)
        for down in (True, False):
            qmp.execute('input-send-event', {'events':[
                {'type':'btn','data':{'button':'left','down':down}}]})
            time.sleep(.08)
        move(630,470)
        if trace_ioctl:
            # The observer starts 20 s after loading; this window is inside
            # its 30 s sample. Trace output is deferred until both finish.
            send('(sleep 10; echo IOCTL_""ACTIVE; echo 1 > /sys/kernel/tracing/tracing_on; sleep 10; '
                 'echo 0 > /sys/kernel/tracing/tracing_on; echo IOCTL_""INACTIVE) &')
        os.environ['CAPTURE_DIR'] = str(out)
        if measure_kvm:
            # Like the Pacha runner, leave the pointer away from the demo and
            # count host activity over a separate, approximately 30 s window.
            time.sleep(6)
            (out/'kvm-schema.json').write_text(json.dumps(qmp.execute(
                'query-stats-schemas', {'provider':'kvm'}), indent=2))
            cpu_before = cpu_snapshot()
            kvm_snapshot('fishbowl-before')
        os.environ['CAPTURE_SECONDS'] = '6,18,30' if measure_kvm else '6,18,30,42'
        runpy.run_path(str(root/'tests/qemu_xfce_x11_capture.py'))
        if measure_kvm:
            kvm_snapshot('fishbowl-after')
            (out/'host-cpu.json').write_text(json.dumps([cpu_before, cpu_snapshot()], indent=2))
            time.sleep(12)  # Let the guest finish its frame/IO dump, outside the sample.
        csv_command = ('gzip -c /linux/tmp/frame-observer.csv | base64' if clock_trial
                       else 'cat /linux/tmp/frame-observer.csv')
        send('cat /linux/tmp/fishbowl.log; echo CSV_BEGIN; ' + csv_command + '; echo CSV_END')
        wait(b'\r\nCSV_END\r\n', timeout=180 if clock_trial else 60)
        assert b'FRAME_OBSERVER_CONTENT type=GtkButton valid=1' in transcript
        assert b'FRAME_OBSERVER_DONE' in transcript
        if clock_trial:
            assert f'FRAME_CLOCK_TRIAL unlimited={int(unlimited)}'.encode() in transcript
            send('grep gtk-frameclock /linux/proc/$(pidof gtk3-demo)/maps; '
                 'echo CLOCK_MAPS_""DONE')
            wait(b'\r\nCLOCK_MAPS_DONE\r\n')
            assert b'/tmp/gtk-frameclock/libgdk-3.so.0' in transcript
        data = bytes(transcript).split(b'\r\nCSV_BEGIN\r\n',1)[1].split(b'\r\nCSV_END\r\n',1)[0]
        data = gzip.decompress(base64.b64decode(data)) if clock_trial else data.replace(b'\r\n',b'\n')
        (out/'frame-observer.csv').write_bytes(data)
        if unsynced:
            send('chroot /linux /usr/bin/env DISPLAY=:0 xrandr --verbose; '
                 'cat /linux/tmp/linux-xfce.log; echo SETTINGS_""DONE')
            wait(b'\r\nSETTINGS_DONE\r\n')
        if trace_ioctl:
            # Split the marker in the echoed shell input so a terminal line
            # wrap cannot be mistaken for the command's completion output.
            send('echo IOCTL_BEGIN; gzip -c /sys/kernel/tracing/trace | base64; echo IOCTL_""END')
            wait(b'\r\nIOCTL_END\r\n')
            data = bytes(transcript).split(b'\r\nIOCTL_BEGIN\r\n',1)[1].split(b'\r\nIOCTL_END\r\n',1)[0]
            (out/'linux-ioctl.trace').write_bytes(gzip.decompress(base64.b64decode(data)))
            (out/'trace-host-anchors.json').write_text(json.dumps(trace_anchors, indent=2))
        send("grep -E 'glamor|renderer|D3D12' /linux/var/log/Xorg.0.log; uname -a; echo VERIFY_DONE")
        wait(b'\r\nVERIFY_DONE\r\n')
        assert b'glamor X acceleration enabled on virgl (D3D12 (Intel(R) Graphics))' in transcript
    finally:
        if qmp:
            qmp.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        console.close()
