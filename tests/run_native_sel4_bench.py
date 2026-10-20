#!/usr/bin/env python3
"""Stage10-S native loader run; one fresh COW and one explicit VM per invocation."""
import argparse, hashlib, json, os, re, subprocess, sys, threading, time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from qemu_xfce_gui_perf import VM,type_text
from benchmark_filed_baseline import overlay

def parse(text):
    text=re.sub(r'^\d+\.\d+ ', '', text, flags=re.M)
    text=re.sub(r'\[Thread \d+\] ','',text)
    if 'S10S_FAIL' in text:raise ValueError('native failure')
    rows=[]
    for line in text.splitlines():
        if not line.startswith('S10S_ROW '):continue
        fields=dict(re.findall(r'(\w+)=([^ ]+)',line))
        rows.append({k:v if k=='test' else int(v) for k,v in fields.items()})
    expected={('overhead',0,0):110,('fault',0,0):110}
    for a in (0,1):
        for w in (0,4):expected[('ipc',a,w)]=256
        expected[('signal',a,0)]=110;expected[('yield',a,0)]=110
    for key,n in expected.items():
        r=[x for x in rows if (x['test'],x['as'],x['payload'])==key]
        if [x['i'] for x in r]!=list(range(n)):raise ValueError(f'missing/duplicate {key}: {len(r)}')
        for x in r:
            if x['t3']<x['t0']:raise ValueError('TSC reversed')
            if x['test'] not in ('overhead','yield') and not x['t0']<=x['t1']<=x['t2']<=x['t3']:raise ValueError('endpoint stamp ordering')
    if len(rows)!=sum(expected.values()) or 'S10S_PASS' not in text:raise ValueError('incomplete')
    return rows

def main():
    p=argparse.ArgumentParser();p.add_argument('--cpus',type=int,choices=[1,4],required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    out=a.out.resolve();out.parent.mkdir(parents=True,exist_ok=True)
    if out.exists():raise RuntimeError('run already exists')
    for proc in Path('/proc').glob('[0-9]*'):
        try:cmd=(proc/'cmdline').read_bytes().split(b'\0')[0]
        except OSError:continue
        if b'qemu-system-' in cmd:raise RuntimeError('another QEMU active')
    old=ROOT/'.artifacts/kernel-refactor-stage10';binary=old/'s/native_sel4_bench.elf';boot=old/'off-boot.img';backing=ROOT/'.artifacts/kernel-refactor-stage7/fix-prepared.qcow2'
    sys.path.insert(0,str(ROOT/'.artifacts/kernel-refactor-stage1'))
    from prepare_u1_boot import verify
    pre=verify(boot,old/'off.elf')
    if not pre['valid']:raise RuntimeError('boot preflight')
    digest=hashlib.sha256(binary.read_bytes()).hexdigest()
    script=(f'wget -q -O /cmd/native_fd_growth.elf http://10.0.2.2:18087/native\n'
            f'echo "{digest}  /cmd/native_fd_growth.elf" | sha256sum -c - || exit 1\n'
            'echo S10S_UPLOAD_OK\ncat /var/log/Xorg.0.log\nsleep 10\n'
            'echo S7_FIX_NATIVE_AFTER_FULL > /tmp/s7fix-native-go\n').encode()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            body=binary.read_bytes() if self.path=='/native' else script if self.path=='/go' else None
            if body is None:self.send_error(404);return
            self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    server=ThreadingHTTPServer(('0.0.0.0',18087),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    disk=out.with_suffix('.qcow2');overlay(disk,backing,'qcow2')
    os.environ.update(CAPOS_QEMU='/usr/bin/qemu-system-x86_64',GALLIUM_DRIVER='d3d12',GDK_BACKEND='x11',MESA_D3D12_DEFAULT_ADAPTER_NAME='Intel')
    vm=VM(str(out),cpus=a.cpus,overlay=disk,disk=backing,boot=boot,graphics='virgl',display='gtk,gl=on',keep_overlay=True,boot_framebuffer=True,memory='4G')
    result=dict(ok=False,cpus=a.cpus,native_sha256=digest,kernel_sha256=hashlib.sha256((old/'off.elf').read_bytes()).hexdigest(),background_syscalls=None,background_reason='Normal off has no exported ipc_syscall_counts; no probes added',boot_preflight=pre)
    def logs():return '\n'.join((out/(n+'.log')).read_text(errors='replace') if (out/(n+'.log')).exists() else '' for n in ['serial','console'])
    def wait(pattern,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            t=logs();m=re.search(pattern,t,re.M)
            if m:return m
            if vm.proc.poll() is not None or vm.fault.is_set() or 'S10S_FAIL' in t:raise RuntimeError('guest failure')
            time.sleep(.2)
        raise TimeoutError(pattern)
    def send(command):
        for c in command:type_text(vm.qmp,c);time.sleep(.20)
        vm.qmp.key('ret')
    try:
        wait(f'boot: cpus ready count={a.cpus}',180);wait('X.Org X Server',240);time.sleep(75)
        vm.qmp.chord('ctrl','alt','t');time.sleep(30)
        for attempt in range(3):
            vm.qmp.chord('ctrl','c');vm.qmp.chord('ctrl','u');send('echo S10S_READY > /dev/hvc0')
            try:wait(r'^S10S_READY\r?$',25);break
            except TimeoutError:pass
        else:raise RuntimeError('no shell handshake')
        send('wget -q -O /tmp/s10s http://10.0.2.2:18087/go');send('sh /tmp/s10s > /dev/hvc0 2>&1')
        wait(r'^S10S_UPLOAD_OK\r?$',60)
        renderer=wait(r'glamor X acceleration enabled on (.+)',30)[1].strip();result['renderer']=renderer
        if 'D3D12 (Intel' not in renderer or re.search('llvmpipe|softpipe',renderer,re.I):raise RuntimeError('renderer')
        wait('S10S_PASS',600);wait('S7_FIX_NATIVE_DONE status=0',60)
        # Probe logs are serial-only; avoid duplicate console copies.
        rows=parse((out/'serial.log').read_text(errors='replace'));(out/'raw.json').write_text(json.dumps(rows,indent=2)+'\n')
        result.update(ok=True,rows=len(rows))
    except Exception as e:result['error']=repr(e);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');vm.close();server.shutdown();server.server_close()
if __name__=='__main__':main()
