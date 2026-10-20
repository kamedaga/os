"""Exact-ELF, stopped-QMP snapshots of profile-only distributions.

GDB writes only the dedicated diagnostic aggregation/epoch symbols, using
QEMU physical-memory mode. Live tokens/locks are never reset. No guest ABI.
"""
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

class RSP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(30)
        self.sock.connect(str(path))
        if self.command('Qqemu.PhyMemMode:1') != b'OK':
            raise RuntimeError('QEMU physical GDB mode unavailable')
    def receive(self,n):
        data=b''
        while len(data)<n:
            chunk=self.sock.recv(n-len(data))
            if not chunk:raise EOFError('GDB closed')
            data+=chunk
        return data
    def command(self, text):
        data=text.encode();self.sock.sendall(b'$'+data+b'#'+f'{sum(data)%256:02x}'.encode())
        while True:
            while self.receive(1)!=b'$': pass
            response=b''
            while True:
                c=self.receive(1)
                if c==b'#': break
                response+=c
            checksum=self.receive(2)
            if int(checksum,16)!=sum(response)%256:raise ValueError('GDB checksum')
            self.sock.sendall(b'+')
            # QMP stop can enqueue an asynchronous GDB stop notification.
            # It is not the response to a physical-memory write/query.
            if response[:1] in (b'T',b'S'):continue
            return response
    def write(self, physical, data):
        # M packets include the address/length prefix AND hex data. 2048 bytes
        # of data alone fill QEMU's 4096-byte packet buffer and leave no room
        # for the prefix. Keep the complete packet comfortably below that cap.
        for offset in range(0,len(data),1024):
            chunk=data[offset:offset+1024]
            if self.command(f'M{physical+offset:x},{len(chunk):x}:{chunk.hex()}')!=b'OK': raise RuntimeError('diagnostic memory write failed')
    def close(self): self.sock.close()

class Profile:
    @classmethod
    def preflight(cls, elf):
        """Validate file-backed metadata before allocating a VM or attaching GDB."""
        reader = cls.__new__(cls)
        reader.elf = Path(elf)
        reader.symbols = {}
        for line in subprocess.check_output(['nm', '-S', str(elf)], text=True).splitlines():
            match = re.fullmatch(r'([0-9a-f]+) ([0-9a-f]+) \S (.+)', line)
            if match:
                reader.symbols[match[3]] = (int(match[1], 16), int(match[2], 16))
        required = ['lock_profile_layout', 'lock_profile_labels', 'lock_profile_epoch',
                    'lock_profile_writers', 'lock_profile_histograms', 'lock_profile_stages',
                    'lock_profile_events', 'lock_profile_free_floor']
        if 'storage_profile_detail' in reader.symbols:
            required += ['storage_profile_layout', 'storage_profile_labels']
        missing = [name for name in required if name not in reader.symbols]
        if missing:
            raise ValueError('missing profile ELF metadata: ' + ', '.join(missing))
        layout = struct.unpack('<7Q', reader.elf_data('lock_profile_layout'))
        _, kinds, stages, series, buckets, stride, stats_offset = layout
        labels = reader.elf_data('lock_profile_labels').rstrip(b'\0').decode().split('\0')
        if len(labels) != kinds + stages or series != kinds * 2 + stages:
            raise ValueError('profile ELF labels/dimensions mismatch')
        if not buckets or stats_offset < buckets * 4 or stride < stats_offset + 48:
            raise ValueError('profile ELF histogram dimensions')
        if 'storage_profile_detail' in reader.symbols:
            owners, storage_kinds, directions, fields = struct.unpack('<4Q', reader.elf_data('storage_profile_layout'))
            owners_labels = reader.elf_data('storage_profile_labels').rstrip(b'\0').decode().split('\0')
            if owners != len(owners_labels) or storage_kinds != kinds or directions != 2 or fields != 5:
                raise ValueError('storage profile ELF dimensions')
        return reader.symbols

    def __init__(self, elf, qmp, gdb, bases, out, resume_after_attach=True):
        self.elf=Path(elf);self.qmp=qmp;self.bases=bases;self.out=Path(out);self.out.mkdir(exist_ok=True)
        self.symbols=self.preflight(elf)
        self.dynamic = "smp_perf_layout" in self.symbols
        self.page_tables = {}
        self.rsp=RSP(gdb);self.epoch=struct.unpack("<Q",self.read("lock_profile_epoch","epoch"))[0];self.sequence=0
        layout=self.read('lock_profile_layout','layout') if self.dynamic else self.elf_data('lock_profile_layout')
        self.cpus,self.kinds,self.stages,self.series,self.buckets,self.stride,self.stats_offset=struct.unpack('<7Q',layout)
        self.labels=self.elf_data('lock_profile_labels').rstrip(b'\0').decode().split('\0')
        if len(self.labels)!=self.kinds+self.stages:raise ValueError('ELF labels do not match layout')
        if not self.dynamic and self.symbols['lock_profile_histograms'][1]!=self.cpus*self.series*self.stride:raise ValueError('ELF extent mismatch')
        self.shapes={'lock_profile_free_floor':self.cpus*8,'lock_profile_histograms':self.cpus*self.series*self.stride,'lock_profile_stages':self.cpus*self.kinds*self.stages*3*8,'lock_profile_events':self.cpus*self.kinds*4*8}
        for key,size in self.shapes.items():
            if self.symbols[key][1]!=(16 if self.dynamic else size):raise ValueError('ELF aggregation extent mismatch')
        if 'pmm_profile_counts' in self.symbols:
            if self.symbols['pmm_profile_counts'][1] != 160: raise ValueError('PMM profile extent mismatch')
            self.shapes['pmm_profile_counts'] = 160
        if 'pmm_profile_aux' in self.symbols:
            if self.symbols['pmm_profile_aux'][1] != 64: raise ValueError('PMM auxiliary extent mismatch')
            self.shapes['pmm_profile_aux'] = 64
        if "pmm_profile_detail" in self.symbols:
            self.shapes["pmm_profile_detail"] = self.cpus * 528 * 8
        if 'storage_profile_detail' in self.symbols:
            owners, kinds, directions, fields = struct.unpack('<4Q', self.elf_data('storage_profile_layout'))
            if kinds != self.kinds or directions != 2 or fields != 5: raise ValueError('storage profile dimensions')
            self.storage_owners = self.elf_data('storage_profile_labels').rstrip(b'\0').decode().split('\0')
            if len(self.storage_owners) != owners: raise ValueError('storage owner labels')
            self.storage_words = owners * kinds * directions * fields
            self.shapes['storage_profile_detail'] = self.cpus * self.storage_words * 8
        # QEMU stops vCPUs on GDB attachment. A new measurement session starts
        # an epoch and resumes before callers send keyboard events. A standalone
        # snapshot instead keeps the stop and preserves the existing window.
        if resume_after_attach:self.begin()
    def elf_data(self,key):
        address,size=self.symbols[key];data=self.elf.read_bytes()
        phoff=struct.unpack_from('<Q',data,32)[0];ents,num=struct.unpack_from('<HH',data,54)
        for i in range(num):
            kind,_,off,va,_,filesz,_,_=struct.unpack_from('<II6Q',data,phoff+i*ents)
            if kind==1 and va<=address and address+size<=va+filesz:return data[off+address-va:off+address-va+size]
        raise ValueError('ELF symbol not file-backed: '+key)
    def physical(self,key):return self.symbols[key][0]-self.bases['Virtual']+self.bases['Physical']
    def read_physical(self,address,size,label):
        path=(self.out/(label+'.bin')).resolve()
        self.qmp.execute('pmemsave',dict(val=address,size=size,filename=str(path)))
        return path.read_bytes()
    def translate(self,virtual):
        # CPU runtime pages are discontiguous. Walk the exact kernel root;
        # the ELF relocation cannot translate these runtime virtual addresses.
        if not hasattr(self,'root_physical'):
            self.root_physical=struct.unpack('<Q',self.read('kernel_cr3_value','root'))[0]&0x000ffffffffff000
        table=self.root_physical
        for shift in (39,30,21,12):
            if table not in self.page_tables:
                self.page_tables[table]=self.read_physical(table,4096,'pt-'+hex(table))
            entry=struct.unpack_from('<Q',self.page_tables[table],((virtual>>shift)&511)*8)[0]
            if not entry&1:raise ValueError('unmapped profile address')
            if shift==12 or (shift in (30,21) and entry&128):
                mask=(1<<shift)-1
                return (entry&0x000ffffffffff000&~mask)+(virtual&mask)
            table=entry&0x000ffffffffff000
        raise AssertionError('page walk')
    def segments(self,key):
        is_slice=key in ('user_copy_detail','storage_profile_detail','pmm_profile_detail','lock_profile_histograms','lock_profile_stages','lock_profile_events','lock_profile_free_floor','lock_profile_writers','smp_perf_counters','munmap_perf_counters','vmo_perf_counters','ipc_perf_counters','ipc_syscall_counts','timer_scan_perf_counters','runtime_path_perf_counters','pt_retire_perf_counters','wake_wait_handoff_perf_counters')
        if not getattr(self,'dynamic',False) or not is_slice:
            return [(self.physical(key),self.symbols[key][1])]
        descriptor=self.read_physical(self.physical(key),16,'descriptor-'+key)
        address,count=struct.unpack('<QQ',descriptor)
        if count!=self.cpus or not address:raise ValueError('CPU slice capacity mismatch')
        size=(self.shapes[key] if key in self.shapes else self.cpus*(128 if key=='smp_perf_counters' else 8))
        segments=[]
        while size:
            length=min(size,4096-(address&4095))
            segments.append((self.translate(address),length))
            address+=length;size-=length
        return segments
    def read(self,key,label):
        data=b''.join(self.read_physical(address,size,label+'-'+key+'-'+str(i)) for i,(address,size) in enumerate(self.segments(key)))
        (self.out/(label+'-'+key+'.bin')).write_bytes(data)
        return data
    def clear(self,key):
        for address,size in self.segments(key):self.rsp.write(address,bytes(size))
    def stop_clean(self):
        for trial in range(100):
            self.qmp.execute('stop')
            writers=self.read('lock_profile_writers','writers')
            if not any(writers):return trial != 0
            self.qmp.execute('cont');time.sleep(.002)
        raise RuntimeError('profile writer never quiescent')
    def reset_stopped(self):
        self.epoch+=1
        for key in self.shapes:self.clear(key)
        self.rsp.write(self.physical('lock_profile_epoch'),struct.pack('<Q',self.epoch))
    def begin(self):
        self.stop_clean()
        try:self.reset_stopped()
        finally:self.qmp.execute('cont')
    def end(self,label):
        invalidated = self.stop_clean()
        try:
            if invalidated:
                self.epoch += 1
                self.rsp.write(self.physical("lock_profile_epoch"), struct.pack("<Q",self.epoch))
                result = dict(invalid=True, reason="snapshot stopped an active writer; repeat entire measurement window", label=label)
                (self.out/(label+"-invalid.json")).write_text(json.dumps(result)+"\n")
                return result
            raw={key:self.read(key,label) for key in self.shapes}
            result=self.decode(raw);result.update(epoch=self.epoch,label=label)
            # Any in-flight sample at this stop must not include the pause.
            self.epoch+=1;self.rsp.write(self.physical('lock_profile_epoch'),struct.pack('<Q',self.epoch))
            (self.out/(label+'.json')).write_text(json.dumps(result,indent=2)+'\n')
            return result
        finally:self.qmp.execute('cont')
    def decode(self,raw):
        records=[];data=raw['lock_profile_histograms']
        for series in range(self.series):
            buckets=[0]*self.buckets;stats=[0]*6
            for cpu in range(self.cpus):
                off=(cpu*self.series+series)*self.stride
                row=struct.unpack_from('<'+str(self.buckets)+'I',data,off)
                values=struct.unpack_from('<6Q',data,off+self.stats_offset)
                buckets=[a+b for a,b in zip(buckets,row)]
                for i,v in enumerate(values):stats[i]=max(stats[i],v) if i in (2,5) else stats[i]+v
            if stats[5]:raise ValueError('bucket overflow: invalid measurement')
            if sum(buckets)!=stats[0]:raise ValueError('inconsistent histogram snapshot')
            name=(self.labels[series//2]+('.hold' if series%2 else '.wait')) if series<self.kinds*2 else self.labels[self.kinds+series-self.kinds*2]
            records.append(dict(name=name,buckets=buckets,count=stats[0],cycles=stats[1],maximum=stats[2],exclusive_cycles=stats[3],excluded=stats[4],quantiles={str(q):quantile(buckets,q) for q in (50,95,99)}))
        stage_data=struct.unpack('<'+str(len(raw['lock_profile_stages'])//8)+'Q',raw['lock_profile_stages'])
        totals={}
        for k in range(self.kinds):
            totals[self.labels[k]]={}
            for st in range(self.stages):
                values=[sum(stage_data[((cpu*self.kinds+k)*self.stages+st)*3+c] for cpu in range(self.cpus)) for c in range(3)]
                totals[self.labels[k]][self.labels[self.kinds+st]]=dict(zip(('count','inclusive_cycles','exclusive_cycles'),values))
        ev=struct.unpack('<'+str(len(raw['lock_profile_events'])//8)+'Q',raw['lock_profile_events'])
        events={self.labels[k]:[sum(ev[(cpu*self.kinds+k)*4+c] for cpu in range(self.cpus)) for c in range(4)] for k in range(self.kinds)}
        floor=struct.unpack('<'+str(self.cpus)+'Q',raw.get('lock_profile_free_floor',bytes(self.cpus*8)))
        minimum=min((2**64-1-v for v in floor if v),default=None)
        result = dict(histograms=records,stages=totals,events=events,minimum_free_pages=minimum)
        if 'pmm_profile_counts' in raw:
            result['pmm'] = list(struct.unpack('<20Q',raw['pmm_profile_counts']))
        if 'storage_profile_detail' in raw:
            words=struct.unpack('<'+str(self.cpus*self.storage_words)+'Q',raw['storage_profile_detail'])
            result['storage_owners']=self.storage_owners
            result['storage_kinds']=self.labels[:self.kinds]
            result['storage_detail']=[sum(words[c*self.storage_words+i] for c in range(self.cpus)) for i in range(self.storage_words)]
        if 'pmm_profile_detail' in raw:
            words=struct.unpack('<'+str(self.cpus*528)+'Q',raw['pmm_profile_detail'])
            result['pmm_detail']=[sum(words[c*528+i] for c in range(self.cpus)) for i in range(528)]
        if 'pmm_profile_aux' in raw:
            result['pmm_aux'] = list(struct.unpack('<8Q',raw['pmm_profile_aux']))
        return result

def bounds(i):
    if i<16:return [i,i]
    e=4+(i-16)//8;sub=(i-16)%8
    lo=(1<<e)+(sub<<(e-3));return [lo,lo+(1<<(e-3))-1]
def quantile(buckets,q):
    count=sum(buckets)
    if not count:return None
    rank=(count*q+99)//100;acc=0
    for i,n in enumerate(buckets):
        acc+=n
        if acc>=rank:return bounds(i)
    raise AssertionError('quantile rank')

def read_counter_rows(elf, qmp, bases, out, key, columns, label):
    """Exact-ELF legacy or runtime slice snapshot, without stopping the VM.

    Boot seals the runtime mappings before readers attach. Counter contents
    remain live; this helper makes no claim of an atomic multi-CPU snapshot.
    """
    reader=Profile.__new__(Profile)
    reader.elf=Path(elf);reader.qmp=qmp;reader.bases=bases;reader.out=Path(out)
    reader.symbols={}
    for line in subprocess.check_output(['nm','-S',str(elf)],text=True).splitlines():
        m=re.fullmatch(r'([0-9a-f]+) ([0-9a-f]+) \S (.+)',line)
        if m:reader.symbols[m[3]]=(int(m[1],16),int(m[2],16))
    if key not in reader.symbols:raise ValueError('missing diagnostic symbol: '+key)
    reader.dynamic=('smp_perf_layout' in reader.symbols or 'ipc_perf_layout' in reader.symbols) and key!='unmap_skip_counts'
    reader.page_tables={}
    if reader.dynamic:
        _,reader.cpus=struct.unpack('<QQ',reader.read_physical(reader.physical(key),16,label+'-descriptor'))
        reader.shapes={key:reader.cpus*columns*8}
    else:
        size=reader.symbols[key][1]
        if size%(columns*8):raise ValueError('invalid diagnostic row stride')
        reader.cpus=size//(columns*8)
    data=reader.read(key,label)
    words=struct.unpack('<'+str(reader.cpus*columns)+'Q',data)
    return [sum(words[cpu*columns+i] for cpu in range(reader.cpus)) for i in range(columns)]
