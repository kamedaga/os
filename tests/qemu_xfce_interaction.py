import hashlib, json, os, runpy, select, socket, struct, sys, threading, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_xfce_apk_add_smoke import QMP
from qemu_xfce_gui_perf import type_text

out = Path(os.environ['INTERACTION_OUT'])
qmp = QMP(str(out/'qmp.sock'))
assert qmp.execute('query-kvm')['enabled']
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
    results.append(row)
    (out/'results.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(row),flush=True)
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

deadline=time.monotonic()+80
matches=0
while time.monotonic()<deadline:
    f=frame()
    matches=matches+1 if hashlib.sha256(f[2][60*stride:]).hexdigest()=='38c8e1630b19a1e08588df39d7ffd83317db59cb8d053464a2318dcdc16b4836' else 0
    if matches==3: break
    time.sleep(.5)
assert matches==3, 'desktop did not match known wallpaper/icons/panel'
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
type_text(qmp,'echo FISHBOWL_START > /dev/hvc0; gtk3-demo --run=fishbowl > /dev/hvc0 2>&1 &')
qmp.key('ret'); time.sleep(12)
assert 'FISHBOWL_START' in Path(os.environ['PACGO_QEMU_CONSOLE_LOG']).read_text(errors='replace'), 'terminal command was not executed'
if os.environ.get('FISHBOWL_BUTTON') == '1':
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
samples=[]; start=time.monotonic()
next_snapshot = 6
while time.monotonic()-start<30:
    f=frame()
    samples.append([f[0]-start,f[1]-start,zlib.crc32(crop(f,(5,65,620,455))),zlib.crc32(crop(f,(214,385,590,446)))])
    if time.monotonic()-start >= next_snapshot:
        save('fishbowl-'+str(next_snapshot),f)
        next_snapshot += 6
    time.sleep(.04)
(out/'host-cpu.json').write_text(json.dumps([cpu_before,cpu_snapshot()],indent=2))
save('fishbowl-after',frame())
(out/'fishbowl-samples.json').write_text(json.dumps(samples))
assert any(a[3]!=b[3] for a,b in zip(samples,samples[1:])), 'fishbowl region did not animate'
record(dict(event='displayed-content-changes-not-app-fps',samples=len(samples),
    changes=sum(a[2]!=b[2] for a,b in zip(samples,samples[1:])),
    fish_region_changes=sum(a[3]!=b[3] for a,b in zip(samples,samples[1:])),seconds=samples[-1][1]-samples[0][0]))
time.sleep(8)
qmp.close()
