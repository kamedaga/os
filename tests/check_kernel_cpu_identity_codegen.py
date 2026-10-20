#!/usr/bin/env python3
"""Audit the actual ELF's CPU-identity call tree used with live user XState.

This is a deliberately limited machine-code gate, not proof of every user
entry/return instruction. Symbol-bearing production ELFs are required; a
missing root or unresolvable transfer fails instead of silently reducing scope.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOTS = ('userReturnCr3ForCurrentCpuFromAsm', 'scheduler_connection.currentThread',
         'scheduler_connection.currentContextForXState',
         'scheduler_connection.currentCr3', 'traps.boundedCurrentCpuSlot')


def parse_disassembly(text):
    bodies = {}
    names = {}
    current = None
    for line in text.splitlines():
        header = re.fullmatch(r'([0-9a-f]+) <(.+)>:', line)
        if header:
            current = int(header[1], 16)
            names[header[2]] = current
            bodies[current] = []
            continue
        instruction = re.match(r'^\s*([0-9a-f]+):\s*(\S+)\s*(.*)$', line)
        if current is not None and instruction:
            bodies[current].append((int(instruction[1], 16), instruction[2], instruction[3]))
    return bodies, names


def audit(bodies, names, roots=ROOTS):
    errors = []
    pending = []
    for root in roots:
        if root not in names:
            errors.append(f'missing audit root: {root}')
        else:
            pending.append(names[root])
    owner = {pc: start for start, body in bodies.items() for pc, _, _ in body}
    visited = set()
    while pending:
        start = pending.pop()
        if start in visited:
            continue
        visited.add(start)
        if not bodies.get(start):
            errors.append(f'empty audit body: {start:x}')
            continue
        for pc, opcode, operands in bodies[start]:
            # Even a constant/aggregate move can destroy interrupted XMM0.
            # Ban all XState access here, including implicit-register x87,
            # MXCSR, AVX zeroing and state save/restore instructions.
            if (re.search(r'%(?:[xyzt]mm\d+|mm\d+|bnd\d+|st(?:\(\d+\))?|k[0-7])\b', operands)
                    or opcode.startswith(('f', 'v', 'xsave', 'xrstor', 'tile', 'bnd'))
                    or opcode in ('ldmxcsr', 'stmxcsr', 'emms', 'ldtilecfg', 'sttilecfg',
                                  'rdpkru', 'wrpkru', 'xgetbv', 'xsetbv')):
                errors.append(f'{pc:x}: XState instruction in identity tree: {opcode} {operands}')
            if opcode.startswith(('call', 'j', 'loop')):
                target = re.match(r'^([0-9a-f]+)\s*(?:<.*>)?$', operands)
                if not target or int(target[1], 16) not in owner:
                    errors.append(f'{pc:x}: unresolved control transfer: {opcode} {operands}')
                    continue
                destination = owner[int(target[1], 16)]
                if destination != start:
                    pending.append(destination)
    return errors, sorted(visited)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    assembly = subprocess.check_output(
        ['objdump', '-d', '--no-show-raw-insn', str(args.elf)], text=True)
    bodies, names = parse_disassembly(assembly)
    errors, visited = audit(bodies, names)
    report = dict(elf=str(args.elf), sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                  roots=list(ROOTS), checked_bodies=[name for name, pc in names.items() if pc in visited],
                  checked_instructions=sum(len(bodies[pc]) for pc in visited), errors=errors,
                  scope='CPU identity closure only; not every XState entry/return path')
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'CPU identity live-XState codegen: {len(visited)} bodies / '
          f'{report["checked_instructions"]} instructions PASS')


if __name__ == '__main__':
    main()
