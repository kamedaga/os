import struct
import unittest
from kernel_lock_profile import Profile, bounds, quantile

class Tests(unittest.TestCase):
    def test_missing_storage_metadata_rejected_before_gdb_attachment(self):
        from pathlib import Path
        from tempfile import TemporaryDirectory
        from unittest.mock import patch
        names = ['lock_profile_layout', 'lock_profile_labels', 'lock_profile_epoch',
                 'lock_profile_writers', 'lock_profile_histograms', 'lock_profile_stages',
                 'lock_profile_events', 'lock_profile_free_floor', 'storage_profile_detail',
                 'storage_profile_labels']
        symbols = '\n'.join('0000000000001000 0000000000000010 D ' + name for name in names)
        with TemporaryDirectory() as out, patch('kernel_lock_profile.subprocess.check_output', return_value=symbols), patch('kernel_lock_profile.RSP') as rsp:
            with self.assertRaisesRegex(ValueError, 'storage_profile_layout'):
                Profile(Path('missing-metadata.elf'), None, None, {}, out)
            rsp.assert_not_called()

    def test_dynamic_cpu_slices_walk_and_clear_discontiguous_pages(self):
        from types import SimpleNamespace
        p=Profile.__new__(Profile)
        p.dynamic=True;p.cpus=72;p.shapes={};p.page_tables={};p.root_physical=0x1000
        va=0xffffff8040000000
        pages={address:bytearray(4096) for address in (0x1000,0x2000,0x3000,0x4000)}
        for table,address,shift in ((0x1000,0x2000,39),(0x2000,0x3000,30),(0x3000,0x4000,21)):
            struct.pack_into('<Q',pages[table],((va>>shift)&511)*8,address|3)
        for i,address in enumerate((0x9000,0xb000,0xf000)):
            struct.pack_into('<Q',pages[0x4000],i*8,address|3)
        descriptor=struct.pack('<QQ',va,72)
        p.physical=lambda key:0x5000
        p.read_physical=lambda address,size,label:descriptor if address==0x5000 else bytes(pages[address])
        self.assertEqual(p.segments('smp_perf_counters'),[(0x9000,4096),(0xb000,4096),(0xf000,1024)])
        writes=[];p.rsp=SimpleNamespace(write=lambda address,data:writes.append((address,len(data))))
        p.clear('smp_perf_counters')
        self.assertEqual(writes,[(0x9000,4096),(0xb000,4096),(0xf000,1024)])
        p.cpus=71
        with self.assertRaises(ValueError):p.segments('smp_perf_counters')
    def test_user_copy_detail_reads_rows_instead_of_descriptor(self):
        from pathlib import Path
        from tempfile import TemporaryDirectory
        from unittest.mock import patch
        from kernel_lock_profile import read_counter_rows
        # Four 46-kind/16-part rows cross twelve discontiguous runtime pages.
        columns = 46 * 16 * 2
        words = [cpu + 1 for cpu in range(4) for _ in range(columns)]
        raw = struct.pack('<' + str(len(words)) + 'Q', *words)
        va = 0xffffff8040000000
        descriptor = struct.pack('<QQ', va, 4)
        def read_physical(self, address, size, label):
            if address == 0x1000:
                return descriptor[:size]
            page = (address - 0x10000) // 8192
            offset = page * 4096 + (address - 0x10000) % 8192
            return raw[offset:offset + size]
        symbols = '1000 10 D user_copy_detail\n2000 10 D smp_perf_layout'
        with TemporaryDirectory() as out, patch('kernel_lock_profile.subprocess.check_output', return_value=symbols), \
             patch.object(Profile, 'physical', return_value=0x1000), \
             patch.object(Profile, 'translate', side_effect=lambda address: 0x10000 + ((address - va) // 4096) * 8192 + (address & 4095)), \
             patch.object(Profile, 'read_physical', read_physical):
            result = read_counter_rows(Path('test.elf'), None, {}, Path(out), 'user_copy_detail', columns, 'copy')
            self.assertEqual(result, [10] * columns)
            self.assertEqual(len((Path(out) / 'copy-user_copy_detail.bin').read_bytes()), 47104)

    def test_dynamic_cpu_walk_rejects_absent_mapping(self):
        p=Profile.__new__(Profile);p.root_physical=4096;p.page_tables={4096:bytes(4096)}
        with self.assertRaises(ValueError):p.translate(0xffffff8040000000)
    def test_control_markers_require_complete_reply(self):
        from kernel_lock_baseline import has_marker
        self.assertFalse(has_marker('echo S7_CONTROL_READY_1 > /dev/hvc0\n','S7_CONTROL_READY_1'))
        self.assertFalse(has_marker('S7_CONTROL_READY_0\n','S7_CONTROL_READY_1'))
        self.assertTrue(has_marker('serial\nS7_CONTROL_READY_1\r\n','S7_CONTROL_READY_1'))
    def test_workload_command_uses_supported_keyboard_events(self):
        from unittest.mock import Mock, patch
        from kernel_lock_baseline import WORKLOAD_COMMAND, FETCH_COMMAND
        from qemu_xfce_gui_perf import type_text
        qmp=Mock()
        for command in (WORKLOAD_COMMAND,FETCH_COMMAND,'echo S7_CONTROL_READY_0 > /dev/hvc0'):
            qmp.reset_mock()
            with patch('qemu_xfce_gui_perf.time.sleep'):
                type_text(qmp,command)
            self.assertEqual(qmp.key.call_count+qmp.chord.call_count,len(command))
    def test_bucket_ranges(self):
        last=-1
        for i in range(496):
            lo,hi=bounds(i)
            self.assertEqual(lo,last+1);self.assertGreaterEqual(hi,lo);last=hi
        self.assertEqual(last,2**64-1)
    def fixture(self):
        p=Profile.__new__(Profile)
        p.cpus=2;p.kinds=1;p.stages=1;p.series=3;p.buckets=496;p.stride=2032;p.stats_offset=1984;p.labels=['map','copy']
        raw={'lock_profile_histograms':bytearray(p.cpus*p.series*p.stride),'lock_profile_stages':bytes(2*1*1*3*8),'lock_profile_events':bytes(2*1*4*8)}
        return p,raw
    def test_pmm_detail_merges_cpu_rows_and_rejects_wrong_extent(self):
        p,raw=self.fixture()
        words=[0]*(2*528)
        words[5]=7;words[528+5]=11;words[520]=3;words[528+521]=2
        raw['pmm_profile_detail']=struct.pack('<1056Q',*words)
        result=p.decode(raw)['pmm_detail']
        self.assertEqual((len(result),result[5],result[520],result[521]),(528,18,3,2))
        raw['pmm_profile_detail']=bytes(528*8)
        with self.assertRaises(struct.error):p.decode(raw)
    def test_storage_detail_cpu_merge_and_extent(self):
        p,raw=self.fixture()
        p.storage_owners=['identity','invocation'];p.storage_words=20
        words=[0]*40;words[5]=3;words[25]=4
        raw['storage_profile_detail']=struct.pack('<40Q',*words)
        result=p.decode(raw)
        self.assertEqual(result['storage_detail'][5],7)
        self.assertEqual(result['storage_kinds'],['map'])
        raw['storage_profile_detail']=bytes(20*8)
        with self.assertRaises(struct.error):p.decode(raw)
    def test_merge_buckets_not_percentiles(self):
        p,raw=self.fixture();data=raw['lock_profile_histograms']
        for cpu,count,bucket in [(0,99,1),(1,1,30)]:
            off=cpu*p.series*p.stride
            struct.pack_into('<I',data,off+bucket*4,count)
            struct.pack_into('<6Q',data,off+p.stats_offset,count,count*bounds(bucket)[0],bounds(bucket)[0],0,0,0)
        row=p.decode(raw)['histograms'][0]
        self.assertEqual(row['quantiles']['99'],[1,1]);self.assertEqual(row['maximum'],bounds(30)[0])
    def test_incomplete_and_overflow_rejected(self):
        p,raw=self.fixture()
        struct.pack_into('<Q',raw['lock_profile_histograms'],p.stats_offset,1)
        with self.assertRaises(ValueError):p.decode(raw)
        struct.pack_into('<6Q',raw['lock_profile_histograms'],p.stats_offset,0,0,0,0,0,1)
        with self.assertRaises(ValueError):p.decode(raw)
    def test_busy_writer_invalidates_whole_window(self):
        from tempfile import TemporaryDirectory
        class Qmp:
            def __init__(self): self.commands=[]
            def execute(self,name,*args): self.commands.append(name)
        class Rsp:
            def __init__(self):self.writes=[]
            def write(self,address,data):self.writes.append((address,data))
        p=Profile.__new__(Profile);p.qmp=Qmp();p.rsp=Rsp();p.epoch=7
        p.stop_clean=lambda:True;p.physical=lambda key:123
        with TemporaryDirectory() as path:
            from pathlib import Path
            p.out=Path(path)
            result=p.end('busy')
            self.assertTrue(result['invalid']);self.assertEqual(p.epoch,8)
            self.assertEqual(p.rsp.writes,[(123,struct.pack('<Q',8))])
            self.assertEqual(p.qmp.commands,['cont'])
    def test_rsp_handles_stop_notification_and_split_checksum(self):
        import socket, threading
        from kernel_lock_profile import RSP
        client,server=socket.socketpair();client.settimeout(2);server.settimeout(2)
        rsp=RSP.__new__(RSP);rsp.sock=client
        def peer():
            data=b''
            while b'#' not in data:data+=server.recv(1024)
            for packet in (b'T05',b'OK'):
                wire=b'$'+packet+b'#'+f'{sum(packet)%256:02x}'.encode()
                for byte in wire:server.sendall(bytes([byte]))
                self.assertEqual(server.recv(1),b'+')
            server.close()
        worker=threading.Thread(target=peer);worker.start()
        self.assertEqual(rsp.command('M1000,1:00'),b'OK');worker.join();client.close()
    def test_empty(self):self.assertIsNone(quantile([0]*496,99))

    def test_rsp_bulk_write_fits_qemu_packet_and_preserves_bytes(self):
        from kernel_lock_profile import RSP
        rsp=RSP.__new__(RSP);packets=[]
        def command(text):
            self.assertLess(len(text.encode()),4096)
            packets.append(text)
            return b'OK'
        rsp.command=command
        base=0xffff800012345678;data=bytes(range(256))*33+b'end'
        rsp.write(base,data)
        rebuilt=bytearray()
        for packet in packets:
            header,payload=packet[1:].split(':')
            address,length=(int(x,16) for x in header.split(','))
            self.assertEqual(address,base+len(rebuilt))
            chunk=bytes.fromhex(payload);self.assertEqual(len(chunk),length)
            rebuilt.extend(chunk)
        self.assertEqual(rebuilt,data)

if __name__=='__main__':unittest.main()
