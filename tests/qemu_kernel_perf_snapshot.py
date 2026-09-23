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
args = parser.parse_args()
args.out.mkdir(exist_ok=False)
bases = {key: int(value, 16) for key, value in re.findall(
    r'limine: (Physical|Virtual) base:\s+(0x[0-9a-f]+)', args.serial.read_text())}
symbols = subprocess.check_output(['nm', '-n', args.elf], text=True)
snapshot = dict(host_monotonic=time.monotonic(), counters={})
qmp = QMP(args.qmp)
try:
    for name, columns in [('smp_perf_counters', 16), ('munmap_perf_counters', 16),
                          ('vmo_perf_counters', 16), ('ipc_perf_counters', 16),
                          ('ipc_syscall_counts', 256), ('timer_scan_perf_counters', 4),
                          ('runtime_path_perf_counters', 32), ('unmap_skip_counts', 16),
                          ('pt_retire_perf_counters', 16)]:
        match = re.search(r'^([0-9a-f]+) B ' + name + r'$', symbols, re.M)
        if not match:
            continue
        physical = int(match[1], 16) - bases['Virtual'] + bases['Physical']
        path = (args.out / (name + '.bin')).resolve()
        qmp.execute('pmemsave', dict(val=physical, size=64*columns*8, filename=str(path)))
        words = struct.unpack('<' + str(64*columns) + 'Q', path.read_bytes())
        snapshot['counters'][name] = [sum(words[cpu*columns+i] for cpu in range(64))
                                      for i in range(columns)]
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
        def read_va(address, size, label):
            if address >= bases['Virtual']:
                physical = address - bases['Virtual'] + bases['Physical']
            elif 0 < address < 16*1024**3 and address + size <= 16*1024**3:
                physical = address  # Kernel allocKernelSlice uses identity addresses.
            else:
                raise ValueError('unsupported kernel data address')
            path = (args.out / (label + '.bin')).resolve()
            qmp.execute('pmemsave', dict(val=physical, size=size, filename=str(path)))
            return path.read_bytes()
        state_address = int(re.search(r'^([0-9a-f]+) b boot\.entry\.limine_kernel_state_storage$', symbols, re.M)[1], 16)
        state_type, desc_type = 'kernel.KernelState', 'state.types.ProcessDescriptor'
        desc_size = number(entries[structure(desc_type)]['byte_size'])
        extra_address = state_address + offset(state_type, 'process_descriptors_extra')
        shape = read_va(extra_address, 16, 'descriptor-shape-before')
        extra_ptr, extra_len = struct.unpack('<QQ', shape)
        if extra_len > 65536-32:
            raise ValueError('invalid descriptor capacity')
        descriptors = read_va(state_address + offset(state_type, 'process_descriptors'),
                              32*desc_size, 'descriptors-inline')
        if extra_len:
            descriptors += read_va(extra_ptr, extra_len*desc_size, 'descriptors-extra')
        raw = read_va(int(principal_symbol[1], 16), 65536*6*8, 'principal-counts')
        snapshot['principal_counts'] = {
            str(index): list(row) for index, row in enumerate(struct.iter_unpack('<6Q', raw)) if any(row)
        }
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
