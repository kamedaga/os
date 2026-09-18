import hashlib, json, os, re, runpy, select, socket, struct, subprocess, sys, threading, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_xfce_apk_add_smoke import QMP
from qemu_xfce_gui_perf import type_text

out = Path(os.environ['INTERACTION_OUT'])
qmp = QMP(str(out/'qmp.sock'))
assert qmp.execute('query-kvm')['enabled']
(out/'gpu-ioeventfd.json').write_text(json.dumps(qmp.execute('qom-get',
    {'path':'/machine/peripheral/pachagpu', 'property':'ioeventfd'})))
(out/'cpus.json').write_text(json.dumps(qmp.execute('query-cpus-fast')))
cpu=qmp.execute('query-cpus-fast')[0]
(out/'tsc-frequency.json').write_text(json.dumps(qmp.execute('qom-get',{'path':cpu['qom-path'],'property':'tsc-frequency'})))
console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
console.connect(os.environ['PACGO_QEMU_CONSOLE'])
def drain():
    with Path(os.environ['PACGO_QEMU_CONSOLE_LOG']).open('ab') as log:
        while True:
            data = console.recv(65536)
            if not data: return
            log.write(data); log.flush()
threading.Thread(target=drain, daemon=True).start()
if os.environ.get('INTERACTION_START_COMMAND'):
    console.sendall((os.environ['INTERACTION_START_COMMAND'] + '\n').encode())
os.environ['CAPTURE_DIR'] = str(out)
os.environ['CAPTURE_SECONDS'] = '0'
ns = runpy.run_path(str(Path(__file__).with_name('qemu_xfce_x11_capture.py')))
c, x, d, window = (ns[k] for k in ('c','x','d','window'))
width, height = ns['w'].value, ns['h'].value
assert (width, height) == (640,505), (width,height)
stride = width*3+1
results = []
def frame():
    serial = Path('/home/kamer/os/.artifacts/serial-tty-test.log').read_text(errors='replace')
    assert 'USER fault' not in serial and 'KERNEL PANIC' not in serial, 'guest fault; reject measurement'
    start = time.monotonic()
    ptr = x.XGetImage(d,window,0,0,width,height,c.c_ulong(-1).value,2)
    assert ptr
    im = c.cast(ptr,c.POINTER(ns['XImage'])).contents
    data = c.string_at(im.data,im.bytes_per_line*height)
    rows = bytearray()
    for y in range(height):
        row = data[y*im.bytes_per_line:y*im.bytes_per_line+width*4]
        rgb = bytearray(width*3)
        rgb[0::3],rgb[1::3],rgb[2::3] = row[2::4],row[1::4],row[0::4]
        rows.append(0); rows.extend(rgb)
    x.XDestroyImage(ptr)
    return start, time.monotonic(), bytes(rows)
def save(name, f):
    chunk=ns['chunk']
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',width,height,8,2,0,0,0))
    (out/(name+'.png')).write_bytes(png+chunk(b'IDAT',zlib.compress(f[2]))+chunk(b'IEND',b''))
def crop(f, rect):
    x0,y0,x1,y1=rect
    return b''.join(f[2][y*stride+1+x0*3:y*stride+1+x1*3] for y in range(y0,y1))
def record(row):
    row['host_monotonic']=time.monotonic()
    row['host_epoch']=time.time()
    results.append(row)
    (out/'results.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(row),flush=True)
def kernel_snapshot(label):
    ipc_elf = os.environ.get('INTERACTION_IPC_PROFILE_ELF')
    elf = ipc_elf or os.environ.get('INTERACTION_KERNEL_PROFILE_ELF')
    if not elf:
        return
    # Existing per-CPU counters, read without stopping the guest. Snapshots
    # are not globally atomic; use interval deltas, not individual events.
    serial = Path(os.environ['PACGO_QEMU_SERIAL_LOG']).read_text()
    bases = {k:int(v,16) for k,v in re.findall(
        r'limine: (Physical|Virtual) base:\s+(0x[0-9a-f]+)', serial)}
    symbols = subprocess.check_output(['nm', '-n', elf], text=True)
    snapshot = dict(host_monotonic=time.monotonic())
    names = ('ipc_perf_counters',) if ipc_elf else (
        'smp_perf_counters', 'munmap_perf_counters', 'vmo_perf_counters')
    if not ipc_elf and re.search(r'^[0-9a-f]+ B ipc_perf_counters$', symbols, re.M):
        names += ('ipc_perf_counters',)
    for name in names:
        symbol = re.search(r'^([0-9a-f]+) B '+name+r'$', symbols, re.M)
        assert symbol, 'missing diagnostic symbol: '+name
        physical = int(symbol[1],16)-bases['Virtual']+bases['Physical']
        path = out/(label+'-'+name+'.bin')
        qmp.execute('pmemsave', dict(val=physical, size=64*16*8, filename=str(path)))
        words = struct.unpack('<1024Q', path.read_bytes())
        snapshot[name] = [sum(words[cpu*16+i] for cpu in range(64)) for i in range(16)]
    symbol = re.search(r'^([0-9a-f]+) B ipc_syscall_counts$', symbols, re.M)
    if symbol:
        physical = int(symbol[1],16)-bases['Virtual']+bases['Physical']
        path = out/(label+'-ipc-syscall-counts.bin')
        qmp.execute('pmemsave', dict(val=physical, size=64*256*8, filename=str(path)))
        words = struct.unpack('<16384Q', path.read_bytes())
        snapshot['native_syscall_counts'] = [
            sum(words[cpu*256+i] for cpu in range(64)) for i in range(256)]
    if ipc_elf:
        (out/(label+'-counters.json')).write_text(json.dumps(snapshot, indent=2))
        return
    symbol = re.search(r'^([0-9a-f]+) B syscall_entry_counts$', symbols, re.M)
    if symbol:
        physical = int(symbol[1],16)-bases['Virtual']+bases['Physical']
        path = out/(label+'-syscall-entry-counts.bin')
        qmp.execute('pmemsave', dict(val=physical, size=257*257*8, filename=str(path)))
    symbol = re.search(r'^([0-9a-f]+) B lapic_access_counts$', symbols, re.M)
    if symbol:
        physical = int(symbol[1],16)-bases['Virtual']+bases['Physical']
        path = out/(label+'-lapic-access-counts.bin')
        qmp.execute('pmemsave', dict(val=physical, size=6*2*8, filename=str(path)))
    (out/(label+'-counters.json')).write_text(json.dumps(snapshot, indent=2))
def kvm_snapshot(label):
    if os.environ.get('INTERACTION_KVM_STATS') != '1':
        return
    for target in ('vm', 'vcpu'):
        stats = qmp.execute('query-stats',
            {'target':target, 'providers':[{'provider':'kvm'}]})
        (out/(label+'-kvm-'+target+'.json')).write_text(json.dumps(stats, indent=2))
def move(px,py):
    qmp.execute('input-send-event',{'events':[
        {'type':'abs','data':{'axis':'x','value':round(px*32767/639)}},
        {'type':'abs','data':{'axis':'y','value':round(py*32767/479)}}]})
def changed(name, before, rect, start, threshold, timeout=8):
    lower=start
    while time.monotonic()-start<timeout:
        f=frame()
        if sum(a!=b for a,b in zip(before,crop(f,rect)))>threshold:
            save(name,f)
            record(dict(event=name,lower_s=max(0,lower-start),upper_s=f[1]-start,capture_s=f[1]-f[0]))
            return True
        lower=f[0]; time.sleep(.025)
    save(name+'-timeout',frame()); record(dict(event=name,timeout_s=timeout)); return False

# Exclude the initial center cursor and the host-window edge; neither is part
# of desktop readiness. Keep Home/File System and wallpaper, excluding the
# optional application shortcuts farther right on the desktop.
move(450,12)
deadline=time.monotonic()+80
matches=0
while time.monotonic()<deadline:
    move(450,12)
    f=frame()
    ready = (hashlib.sha256(crop(f,(0,60,130,455))).hexdigest() ==
             'b0f7d91af0eaf325cd78ab96889036f3c7e7c72be7efc8d928a6b58d21f999f4' and
             hashlib.sha256(crop(f,(350,60,620,455))).hexdigest() ==
             '47a1fa69db691d96232faf6c82b9e31388ce34c81f263002b49640a3c16d7c0a')
    matches=matches+1 if ready else 0
    if matches==3: break
    time.sleep(.5)
if matches != 3: save('desktop-timeout',f)
assert matches==3, 'desktop did not match known wallpaper/icons'
save('desktop',f)
time.sleep(2)
for i in range(3):
    move(450,350); time.sleep(.7)
    rect=(38,74,81,112); before=crop(frame(),rect)
    start=time.monotonic(); move(60,110)
    changed('home-hover-'+str(i),before,rect,start,20,3)
for i in range(3):
    move(45,12); time.sleep(.3)
    rect=(0,55,260,360); before=crop(frame(),rect)
    start=time.monotonic()
    qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
    time.sleep(.08)
    qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
    changed('applications-'+str(i),before,rect,start,1000)
    qmp.key('esc'); time.sleep(1)
qmp.chord('ctrl','alt','t'); time.sleep(6)
save('terminal-before-command',frame())
type_text(qmp,os.environ.get('FISHBOWL_COMMAND',
    'echo FISHBOWL_START > /dev/hvc0; gtk3-demo --run=fishbowl > /dev/hvc0 2>&1 &'))
qmp.key('ret'); time.sleep(12)
assert 'FISHBOWL_START' in Path(os.environ['PACGO_QEMU_CONSOLE_LOG']).read_text(errors='replace'), 'terminal command was not executed'
if os.environ.get('FISHBOWL_BUTTON') == '1':
    if 'FRAME_OBSERVER_FIXED_COUNT=' in os.environ.get('FISHBOWL_COMMAND', ''):
        assert 'FRAME_OBSERVER_FIXED_BUTTON=1' in os.environ['FISHBOWL_COMMAND'], \
            'fixed Button measurements must verify the actual GTK child type'
    # Use the stock next button: "Button" has fixed contents unlike random Icon.
    move(266,333); time.sleep(.3)
    qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
    time.sleep(.08)
    qmp.execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
    time.sleep(5)
    record(dict(event='stock-fishbowl-next-button-clicked'))
move(630,470); time.sleep(1)
save('fishbowl-before',frame())
record(dict(event='fishbowl-sampling-begin'))
def cpu_snapshot():
    tid=qmp.execute('query-cpus-fast')[0]['thread-id']
    status=Path('/proc/'+str(tid)+'/status').read_text()
    pid=next(line.split()[1] for line in status.splitlines() if line.startswith('Tgid:'))
    rows={}
    for path in Path('/proc/'+pid+'/task').glob('*/stat'):
        data=path.read_text(); fields=data[data.rfind(')')+2:].split()
        rows[path.parent.name]=dict(name=data[data.find('(')+1:data.rfind(')')], ticks=int(fields[11])+int(fields[12]))
    return dict(time=time.monotonic(), hz=os.sysconf('SC_CLK_TCK'), rows=rows)
cpu_before=cpu_snapshot()
if os.environ.get('INTERACTION_KVM_STATS') == '1':
    (out/'kvm-schema.json').write_text(json.dumps(qmp.execute(
        'query-stats-schemas', {'provider':'kvm'}), indent=2))
kvm_snapshot('fishbowl-before')
kernel_snapshot('fishbowl-before')
samples=[]; start=time.monotonic()
register_samples=[]
sample_registers = os.environ.get('INTERACTION_REGISTER_SAMPLES') == '1'
exit_trace = os.environ.get('INTERACTION_KVM_EXIT_TRACE') == '1'
exit_trace_state = 0
exit_trace_events = ('kvm_run_exit', 'kvm_vcpu_ioctl',
                     'memory_region_ops_read', 'memory_region_ops_write',
                     'virtio_gpu_cmd_res_create_3d', 'virtio_gpu_cmd_res_unref',
                     'virtio_gpu_cmd_ctx_submit', 'virtio_gpu_cmd_res_flush')
next_snapshot = 6
while time.monotonic()-start<30:
    elapsed = time.monotonic()-start
    if exit_trace and exit_trace_state == 0 and elapsed >= 12:
        for event in exit_trace_events:
            qmp.execute('trace-event-set-state', {'name':event, 'enable':True})
        exit_trace_state = 1
        record(dict(event='kvm-exit-trace-begin', epoch=time.time()))
    if exit_trace and exit_trace_state == 1 and elapsed >= 15:
        for event in exit_trace_events:
            qmp.execute('trace-event-set-state', {'name':event, 'enable':False})
        exit_trace_state = 2
        record(dict(event='kvm-exit-trace-end', epoch=time.time()))
    f=frame()
    samples.append([f[0]-start,f[1]-start,zlib.crc32(crop(f,(5,65,620,455))),zlib.crc32(crop(f,(214,385,590,446)))])
    if time.monotonic()-start >= next_snapshot:
        save('fishbowl-'+str(next_snapshot),f)
        next_snapshot += 6
    if sample_registers:
        cpu_index = len(register_samples) % 4
        began = time.monotonic()
        registers = qmp.execute('human-monitor-command',
            {'command-line':'info registers', 'cpu-index':cpu_index})
        register_samples.append(dict(start=began, end=time.monotonic(),
            cpu=cpu_index, registers=registers))
    time.sleep(.04)
if sample_registers:
    (out/'register-samples.json').write_text(json.dumps(register_samples))
kernel_snapshot('fishbowl-after')
kvm_snapshot('fishbowl-after')
(out/'host-cpu.json').write_text(json.dumps([cpu_before,cpu_snapshot()],indent=2))
save('fishbowl-after',frame())
(out/'fishbowl-samples.json').write_text(json.dumps(samples))
assert any(a[3]!=b[3] for a,b in zip(samples,samples[1:])), 'fishbowl region did not animate'
record(dict(event='displayed-content-changes-not-app-fps',samples=len(samples),
    changes=sum(a[2]!=b[2] for a,b in zip(samples,samples[1:])),
    fish_region_changes=sum(a[3]!=b[3] for a,b in zip(samples,samples[1:])),seconds=samples[-1][1]-samples[0][0]))
if os.environ.get('INTERACTION_XFWM_DIAGNOSTIC') == '1':
    # Outside the timed window; use the unmodified compositor's own logging.
    qmp.chord('ctrl','alt','t'); time.sleep(3)
    type_text(qmp, 'G_MESSAGES_DEBUG=all LIBGL_DEBUG=verbose xfwm4 --replace > /dev/hvc0 2>&1 &')
    qmp.key('ret'); time.sleep(20)
    save('xfwm-diagnostic', frame())
time.sleep(float(os.environ.get('INTERACTION_POST_SAMPLE_WAIT', '8')))
# The frame table precedes the much larger I/O table. A fixed delay can
# terminate QEMU after the former while silently truncating the latter.
console_path = Path(os.environ['PACGO_QEMU_CONSOLE_LOG'])
if 'FRAME_OBSERVER_FILE=' in os.environ.get('FISHBOWL_COMMAND', ''):
    assert 'FRAME_OBSERVER_READY' in console_path.read_text(errors='replace'), \
        'requested frame observer did not load; reject measurement'
if 'FRAME_OBSERVER_READY' in console_path.read_text(errors='replace'):
    dump_deadline = time.monotonic() + 30
    while 'FRAME_OBSERVER_DONE' not in console_path.read_text(errors='replace'):
        assert time.monotonic() < dump_deadline, 'frame observer dump did not finish'
        time.sleep(.2)
    if 'FRAME_OBSERVER_FIXED_BUTTON=1' in os.environ.get('FISHBOWL_COMMAND', ''):
        assert 'FRAME_OBSERVER_CONTENT type=GtkButton valid=1' in console_path.read_text(), \
            'fishbowl workload was not verified as one GtkButton'
qmp.close()
