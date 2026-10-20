#!/usr/bin/env python3
"""Reject SIMD/x87 in the whole kernel image, including indirect callees.

The only exceptions are explicit state transfer/initialization instructions at
reviewed boundaries. No function body is exempt. This stronger image-wide rule
avoids pretending that an unresolved indirect edge has no reachable code.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from check_kernel_cpu_identity_codegen import parse_disassembly

BOUNDARIES = {
    'restoreCurrentThreadXState': {'xrstor64'},
    'saveCurrentThreadXState': {'xsave64', 'xsaveopt64'},
    'smp.enterUserModeFromIdle': {'xrstor64'},
    'arch.x86_64.platform.enableAvxXStateForCurrentCpu': {'fninit'},
    'xstate_runtime.initXStateSupport': {'xsave64'},
    'arch.x86_64.platform.validateUserXState': {'fxsave64'},
    **{name: {'fxsave64', 'fxrstor64'} for name in (
        'deviceInterruptHandlerStub', 'schedulerWakeIpiHandlerStub',
        'timerInterruptHandlerStub')},
}
ROOTS = ('syscallDirectIpcEntry', 'syscallDirectIpcPrepare', 'syscallDirectIpcCommit', 'syscallEntryCommon', 'syscallIpcScalarDispatch', 'finishIpcLive',
         'saveCurrentThreadXState', 'restoreCurrentThreadXState')

def audit(bodies, names, roots=ROOTS):
    errors = [f'missing root: {name}' for name in roots if name not in names]
    boundaries = []
    for name, start in names.items():
        for pc, op, args in bodies[start]:
            if re.search(r'%(?:[xyzt]mm\d+|mm\d+|st(?:\(\d+\))?|k[0-7]|bnd\d+)\b', args):
                errors.append(f'{name}+{pc-start:x}: SIMD register: {op} {args}')
            elif (op.startswith(('f', 'v', 'xsave', 'xrstor', 'tile', 'bnd')) or
                  op in ('ldmxcsr', 'stmxcsr', 'emms', 'ldtilecfg', 'sttilecfg')):
                if op not in BOUNDARIES.get(name, set()):
                    errors.append(f'{name}+{pc-start:x}: non-scalar instruction: {op} {args}')
                else:
                    boundaries.append(dict(function=name, address=hex(pc), opcode=op))
    return errors, boundaries

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=Path)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    b, n = parse_disassembly(subprocess.check_output(
        ['objdump', '-d', '--no-show-raw-insn', str(a.elf)], text=True))
    errors, boundaries = audit(b, n)
    report = dict(sha256=hashlib.sha256(a.elf.read_bytes()).hexdigest(),
                  bodies=len(b), instructions=sum(map(len,b.values())),
                  errors=errors, state_boundaries=boundaries,
                  scope='whole linked kernel, including IRQ/exception and indirect callees')
    if a.output: a.output.write_text(json.dumps(report, indent=2)+'\n')
    if errors: raise SystemExit('\n'.join(errors))
    print(f'Kernel scalar codegen: {len(b)} bodies, SIMD/x87 arithmetic=0 PASS')
if __name__ == '__main__': main()
