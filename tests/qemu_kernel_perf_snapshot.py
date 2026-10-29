#!/usr/bin/env python3
"""Read existing per-CPU kernel counters without pausing a running VM.

The ELF must be the exact running diagnostic kernel. Snapshots are not globally
atomic; use interval deltas. Counters cover all processes, not a browser alone.
"""
import argparse
import functools
import json
from pathlib import Path
import re
import struct
import subprocess
import time

from qemu_xfce_apk_add_smoke import QMP
from qemu_ipc_channel_snapshot import dwarf_entries, name as dwarf_name, child, number

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qmp', required=True)
parser.add_argument('--elf', required=True)
parser.add_argument('--serial', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--lock-profile', choices=['begin', 'snapshot'], help='Use exact-ELF stage7 histograms instead of legacy counters')
parser.add_argument('--gdb', help='QEMU GDB Unix socket for stopped diagnostic epoch writes')
args = parser.parse_args()
args.out.mkdir(exist_ok=False)
bases = {key: int(value, 16) for key, value in re.findall(
    r'limine: (Physical|Virtual) base:\s+(0x[0-9a-f]+)', args.serial.read_text())}
symbols = subprocess.check_output(['nm', '-n', args.elf], text=True)
snapshot = dict(host_monotonic=time.monotonic(), counters={})
qmp = QMP(args.qmp)
try:
    if args.lock_profile:
        if not args.gdb:
            raise ValueError('--lock-profile requires --gdb')
        from kernel_lock_profile import Profile
        profile = Profile(args.elf, qmp, args.gdb, bases, args.out, resume_after_attach=False)
        try:
            if args.lock_profile == 'begin':
                profile.begin()
                snapshot['epoch'] = profile.epoch
            else:
                snapshot['distributions'] = profile.end('snapshot')
            (args.out/'counters.json').write_text(json.dumps(snapshot,indent=2)+'\n')
        finally:
            profile.rsp.close()
        print(json.dumps(dict(out=str(args.out), mode=args.lock_profile)))
        raise SystemExit(0)
    # The diagnostic row grew as IPC stages were split. Read its exact-ELF
    # layout rather than silently applying the old sixteen-word CPU stride.
    ipc_columns = 16
    layout = re.search(r'^([0-9a-f]+) [BD] ipc_perf_layout$', symbols, re.M)
    if layout:
        address = int(layout[1], 16) - bases['Virtual'] + bases['Physical']
        path = (args.out / 'ipc-layout.bin').resolve()
        qmp.execute('pmemsave', dict(val=address, size=24, filename=str(path)))
        _, ipc_columns, syscall_columns = struct.unpack('<3Q', path.read_bytes())
        if not 0 < ipc_columns <= 4096 or ipc_columns % 2 or syscall_columns != 256:
            raise ValueError('invalid IPC diagnostic layout')
    for name, columns in [('smp_perf_counters', 16), ('munmap_perf_counters', 16),
                          ('vmo_perf_counters', 16), ('ipc_perf_counters', ipc_columns),
                          ('ipc_syscall_counts', 256), ('timer_scan_perf_counters', 4),
                          ('runtime_path_perf_counters', 96 if 'smp_perf_layout' in symbols else 32), ('unmap_skip_counts', 16),
                          ('pt_retire_perf_counters', 16)]:
        if not re.search(r'^[0-9a-f]+ [BD] '+name+r'$',symbols,re.M):continue
        from kernel_lock_profile import read_counter_rows
        snapshot['counters'][name]=read_counter_rows(args.elf,qmp,bases,args.out,name,columns,'snapshot')
    principal_symbol = re.search(r'^([0-9a-f]+) B ipc_principal_syscall_counts$', symbols, re.M)
    if principal_symbol:
        # Decode the exact ELF's layout, not guessed Zig struct offsets.
        entries = dwarf_entries(args.elf)
        @functools.cache
        def structure(type_name):
            return next(key for key, value in entries.items()
                        if value['tag'] == 'structure_type' and dwarf_name(value) == type_name)
        @functools.cache
        def offset(type_name, field):
            return number(child(entries, structure(type_name), field)['data_member_location'])
        from kernel_lock_profile import Profile
        reader = Profile.__new__(Profile)
        reader.elf, reader.qmp, reader.bases, reader.out = Path(args.elf), qmp, bases, args.out
        reader.symbols = {match[3]: (int(match[1], 16), int(match[2], 16))
            for match in re.finditer(r'^([0-9a-f]+) ([0-9a-f]+) \S (.+)$',
                subprocess.check_output(['nm', '-S', args.elf], text=True), re.M)}
        reader.page_tables = {}
        def read_va(address, size, label):
            # Descriptor storage can live outside the ELF mapping. Walk the
            # actual kernel page tables, including discontiguous allocations.
            chunks = []
            while size:
                count = min(size, 4096 - (address & 4095))
                chunks.append(reader.read_physical(reader.translate(address), count,
                    label + '-' + str(len(chunks))))
                address += count
                size -= count
            return b''.join(chunks)
        state_address = int(re.search(r'^([0-9a-f]+) b boot\.entry\.limine_kernel_state_storage$', symbols, re.M)[1], 16)
        state_type, desc_type = 'kernel.KernelState', 'state.types.ProcessDescriptor'
        desc_size = number(entries[structure(desc_type)]['byte_size'])
        extra_address = state_address + offset(state_type, 'process_descriptors_extra')
        shape = read_va(extra_address, 16, 'descriptor-shape-before')
        extra_ptr, extra_len = struct.unpack('<QQ', shape)
        if extra_len > (1 << 31)-32:
            raise ValueError('invalid descriptor capacity')
        descriptors = read_va(state_address + offset(state_type, 'process_descriptors'),
                              32*desc_size, 'descriptors-inline')
        if extra_len:
            descriptors += read_va(extra_ptr, extra_len*desc_size, 'descriptors-extra')
        counts_offset = offset(desc_type, 'syscall_counts')
        snapshot['principal_counts'] = {}
        for index in range(32 + extra_len):
            row = struct.unpack_from('<6Q', descriptors, index*desc_size + counts_offset)
            if any(row):
                snapshot['principal_counts'][str(index)] = list(row)
        snapshot['descriptor_shape_stable'] = shape == read_va(extra_address, 16, 'descriptor-shape-after')
        snapshot['descriptors'] = {}
        label_cache = {}
        for index in snapshot['principal_counts']:
            i = int(index)
            if i >= 32 + extra_len:
                continue
            data = descriptors[i*desc_size:(i+1)*desc_size]
            active = bool(data[offset(desc_type, 'active')])
            principal = struct.unpack_from('<I', data, offset(desc_type, 'principal'))[0]
            generation = struct.unpack_from('<I', data, offset(desc_type, 'generation'))[0]
            record = dict(active=active, principal=principal, generation=generation)
            if active:
                ptr, length = struct.unpack_from('<QQ', data, offset(desc_type, 'label'))
                if ptr and 0 < length <= 256:
                    if (ptr, length) not in label_cache:
                        label_cache[ptr, length] = read_va(ptr, length, 'label-'+index).decode(errors='replace')
                    record['label'] = label_cache[ptr, length]
            snapshot['descriptors'][index] = record
finally:
    qmp.close()
snapshot['snapshot_seconds'] = time.monotonic() - snapshot['host_monotonic']
(args.out / 'counters.json').write_text(json.dumps(snapshot, indent=2))
print(json.dumps(dict(out=str(args.out), snapshot_seconds=snapshot['snapshot_seconds'],
                      groups=list(snapshot['counters']),
                      principal_slots=len(snapshot.get('principal_counts', {})))))
