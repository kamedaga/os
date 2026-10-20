"""One-boot contention phases, served from host without modifying rootfs."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import json
import threading
import time
import re

FETCH_COMMAND = 'wget -q -O /tmp/s7 http://10.0.2.2:18087/s7 && chmod +x /tmp/s7; echo S7_FETCH=$? > /dev/hvc0'
WORKLOAD_COMMAND = '/tmp/s7 > /dev/hvc0 2>&1; echo S7_EXIT=$? > /dev/hvc0'

def has_marker(text, marker):
    # Echoed shell input and separately growing serial/console are not replies.
    return re.search(r'^'+re.escape(marker)+r'\r?$',text,re.M) is not None

def run_contention(vm, profile, binary, output, type_text, logs):
    from kernel_tlb_profile import attach
    attach(profile, output.parent / "smp-after-full.json")
    def wait_marker(marker, seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if has_marker(logs(),marker):return
            if vm.fault.is_set() or vm.proc.poll() is not None:
                raise RuntimeError('guest fault/exit during control handshake')
            time.sleep(.2)
        raise TimeoutError(marker)
    # Snapshot resume does not prove that the foreground shell is ready.
    time.sleep(2)
    for attempt in range(3):
        marker=f'S7_CONTROL_READY_{attempt}'
        vm.qmp.chord('ctrl','u')
        type_text(vm.qmp,f'echo {marker} > /dev/hvc0');vm.qmp.key('ret')
        try:
            wait_marker(marker,20)
            break
        except TimeoutError:
            print(f'control handshake retry {attempt}',flush=True)
    else:raise RuntimeError('contention shell handshake failed')
    rows=[];failure=[];last_ready=[None];retries={}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            try:
                request=urlparse(self.path)
                if request.path=='/s7':body=binary.read_bytes()
                else:
                    q={k:int(v[0]) for k,v in parse_qs(request.query).items()};i=q['id']
                    if request.path=='/ready':
                        if i!=len(rows) or last_ready[0] is not None:raise ValueError('phase ordering')
                        profile.begin();last_ready[0]=i
                    elif request.path=='/done':
                        if last_ready[0]!=i:raise ValueError('unpaired phase')
                        snapshot=profile.end(f'phase-{i:02d}')
                        if snapshot.get('invalid'):
                            last_ready[0]=None;retries[i]=retries.get(i,0)+1
                            if retries[i]>3:raise RuntimeError('repeated active-writer snapshots')
                            body=b'RETRY\n'
                            self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
                            return
                        q.update(family=('mmap','fork_exec','ipc','fd')[i//18],separate=bool(i//9%2),workers=(1,2,4)[i//3%3],trial=i%3,snapshot=snapshot)
                        rows.append(q);last_ready[0]=None
                        output.write_text(json.dumps(rows,indent=2)+'\n')
                        print(json.dumps({k:v for k,v in q.items() if k!='snapshot'}),flush=True)
                        if q['errors']:raise RuntimeError('guest workload failed')
                    else:raise ValueError('unknown endpoint')
                    body=b'GO\n'
                self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            except Exception as error:
                failure.append(repr(error))
                self.send_error(500,str(error))
    server=ThreadingHTTPServer(('0.0.0.0',18087),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        type_text(vm.qmp,FETCH_COMMAND);vm.qmp.key('ret')
        wait_marker('S7_FETCH=0',60)
        type_text(vm.qmp,WORKLOAD_COMMAND);vm.qmp.key('ret')
        deadline=time.monotonic()+1800
        while time.monotonic()<deadline:
            text=logs()
            if failure:raise RuntimeError(failure[0])
            if has_marker(text,'S7_WORKLOAD_DONE') and has_marker(text,'S7_EXIT=0'):
                if len(rows)!=72:raise RuntimeError('missing contention phases')
                return rows
            if vm.fault.is_set() or vm.proc.poll() is not None:raise RuntimeError('guest fault/exit')
            import re
            if re.search(r'^S7_EXIT=[1-9]\d*\r?$',text,re.M):raise RuntimeError('nonzero guest exit')
            time.sleep(.2)
        raise TimeoutError('contention workload')
    finally:server.shutdown();server.server_close()
