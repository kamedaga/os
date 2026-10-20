#!/usr/bin/env python3
"""Per-function ELF stack gate; direct runtime reachability, not path budgets.

Include entry return addresses, pushes, outgoing call return slots and worst
alignment padding. Follow local branches and absolute ELF jump tables. Stack
switches in hand-written entry code have reviewed, fingerprinted contracts;
no unknown RSP write or dynamic alloca is silently accepted. Indirect callees
are reported, not resolved. Stack high-water/interrupt nesting is a separate
runtime measurement, not a claim made by this gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from collections import deque

from check_kernel_cpu_identity_codegen import parse_disassembly

# Regression ceilings, not a nested-path proof: 32 KiB leaves most of the
# AP 512 KiB kernel stack (and the smaller exception stacks) for callers/IRQs;
# BSP-only boot has a 1 MiB stack and may use the 128 KiB global ceiling.
# High-water measurement must establish the remaining nesting budget.
ALL_LIMIT = 128 * 1024
RUNTIME_LIMIT = 32 * 1024
ROOTS = ('syscallDirectIpcEntry', 'syscallEntryCommon', 'syscallEntryCommonFromKernel', 'syscallEntryCommonKernelReady', 'pageFaultHandlerStub', 'doubleFaultHandlerStub',
         'timerInterruptHandlerStub', 'deviceInterruptHandlerStub',
         'schedulerWakeIpiHandlerStub', 'generalProtectionHandlerStub',
         'invalidTssHandlerStub', 'segmentNotPresentHandlerStub',
         'stackSegmentFaultHandlerStub', 'divideErrorHandlerStub',
         'breakpointHandlerStub', 'invalidOpcodeHandlerStub', 'smp.apIdleEntry',
         # Runtime callback installed by boot, hence also an explicit root.
         'boot.entry.exitCurrentProcess', 'resumeAfterFatalUserException')
BOOT_ROOTS = ('_start', 'kernel_entry.limineKernelMain')
# Verified aggregate adapters copy an entire runqueue; the live scheduler uses
# the small entity API. Freeze that separation independently of frame size.
VERIFIED_AGGREGATE = tuple('pacha_eevdf_' + suffix for suffix in
                           ('add', 'wake', 'block', 'exit', 'charge', 'mark_running', 'requeue_running'))
ERROR_STUBS = ('pageFaultHandlerStub', 'doubleFaultHandlerStub',
               'generalProtectionHandlerStub', 'invalidTssHandlerStub',
               'segmentNotPresentHandlerStub', 'stackSegmentFaultHandlerStub')


def plain(name):
    return re.sub(r'__anon_\d+$', '', name)


def signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def elf_sections(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    offset = struct.unpack_from('<Q', data, 40)[0]
    size, count = struct.unpack_from('<HH', data, 58)
    result = []
    for index in range(count):
        _, kind, flags, address, file_offset, length = struct.unpack_from('<IIQQQQ', data, offset + index * size)
        if kind in (1, 8) and flags & 2:
            # NOBITS needs address bounds for relocations, not allocated bytes.
            contents = data[file_offset:file_offset + length] if kind == 1 else range(length)
            result.append((address, contents, bool(flags & 4)))
    return result


def switch_fingerprint(body, sections):
    """Preserve instructions/constants; normalize only actual relocations."""
    lines = []
    for _, op, args in body:
        args = re.sub(r'__anon_\d+', '', args)
        args = re.sub(r'\b[0-9a-f]+ <([^>]+)>', r'<\1>', args)
        args = re.sub(r'-?0x[0-9a-f]+\(%rip\)', 'RELOC(%rip)', args)
        args = re.sub(r'#\s*[0-9a-f]+', '# RELOC', args)
        def address(match):
            value = int(match[1], 16)
            if any(start <= value < start + len(data) for start, data, _ in sections):
                return '$ELF_ADDRESS'
            return match[0]
        args = re.sub(r'\$0x([0-9a-f]+)', address, args)
        if op == 'lea':
            def displacement(match):
                value = int(match[0], 0) % (1 << 64)
                if any(start <= value < start + len(data) for start, data, _ in sections):
                    return 'ELF_ADDRESS'
                return match[0]
            args = re.sub(r'-?0x[0-9a-f]+(?=\()', displacement, args)
        lines.append(op + ' ' + args)
    return hashlib.sha256('\n'.join(lines).encode()).hexdigest()


def direct_target(args):
    match = re.match(r'^([0-9a-f]+)(?:\s|$)', args)
    return int(match[1], 16) if match else None


def stack_write(op, args):
    parts = args.split('#')[0].strip().split(',')
    # XCHG writes both operands; unsupported implicit stack writers fail too.
    return (parts[-1].strip() in ('%rsp', '%esp', '%sp', '%spl')
            and op not in ('cmp', 'test', 'push', 'pushq')) or (
        op.startswith('xchg') and re.search(r'%[er]?sp\b', args)) or op in ('enter', 'leave', 'leaveq')


def frame(name, body, owner, sections=(), contract=None, table_ends=None):
    code = {pc: (op, args) for pc, op, args in body}
    next_pc = {body[i][0]: body[i + 1][0] for i in range(len(body) - 1)}
    errors, indirect = [], []
    if contract:
        # Opt-in boot painting changes _start before its stack switch. Each
        # accepted body still needs an exact fingerprint and its own indices;
        # never accept an arbitrary RSP write because diagnostics are enabled.
        fingerprint = switch_fingerprint(body, sections)
        candidates = [contract] + contract.get('variants', [])
        matches = [c for c in candidates if c['sha256'] == fingerprint]
        if len(matches) != 1:
            return None, ['reviewed stack-switch body changed'], []
        contract = matches[0]
    switches = {} if contract is None else {int(k): v for k, v in contract['switches'].items()}
    # Hardware enters IST with five words, plus an error word where applicable.
    # Ordinary functions include the caller's return address in their bound.
    initial = 48 if name in ERROR_STUBS else 40 if name.endswith('HandlerStub') else 0 if name in ('syscallEntryCommon', 'syscallEntryCommonFromKernel', 'syscallEntryCommonKernelReady', 'syscallDirectIpcEntry') or name.startswith('traps.SyscallEntryForCpu(') else 8
    indexes = {pc: i for i, (pc, _, _) in enumerate(body)}
    seen, pending = set(), deque([(body[0][0], initial, None, None)])
    maximum = initial
    visited = set()
    while pending:
        pc, depth, rbp, r15 = pending.popleft()
        state = (pc, depth, rbp, r15)
        if state in seen:
            continue
        seen.add(state)
        visited.add(pc)
        if len(seen) > max(1024, len(body) * 64):
            errors.append('non-convergent stack state (unbounded/dynamic frame)')
            break
        op, original = code[pc]
        args = original.split('#')[0].strip()
        before = depth
        if op in ('lcall', 'lcallq', 'sysenter', 'sysexit'):
            errors.append(f'{pc:x}: unmodelled implicit stack switch: {op}')
            continue
        if re.match(r'^(?:push\w*|pop\w*|enter|leave|call\w*|ret\w*|lret\w*)\b', args):
            errors.append(f'{pc:x}: unsupported prefixed stack instruction: {op} {args}')
            continue
        if op in ('data16', 'addr32', 'rex.W', 'rep', 'repz', 'repnz', 'lock', 'cs', 'ss') and (
                re.search(r'\b(?:push\w*|pop\w*|enter|leave|call\w*|ret\w*|lret\w*)\b', args)
                or re.search(r'%[er]?sp\b', args)):
            errors.append(f'{pc:x}: unsupported prefixed stack instruction: {op} {args}')
            continue
        if op == 'lretq':
            # The GDT reload's two immediate pushes return to the next local
            # instruction with the new CS. No arbitrary far-return CFG guess.
            i = indexes[pc]
            previous = body[i - 1] if i >= 1 else (0, '', '')
            selector = body[i - 2] if i >= 2 else (0, '', '')
            target = re.fullmatch(r'\$(0x[0-9a-f]+)', previous[2])
            if previous[1] != 'push' or selector[1:] != ('push', '$0x8') or not target or int(target[1], 16) != next_pc.get(pc):
                errors.append(f'{pc:x}: unmodelled far return')
                continue
            depth -= 16
        elif indexes[pc] in switches:
            # A reviewed switch starts a different stack segment. Count its
            # local peak independently, never subtract unrelated addresses.
            depth = switches[indexes[pc]]['initial_bytes']
            rbp = r15 = None
        elif op in ('push', 'pushq', 'pushf', 'pushfq'):
            depth += 8
        elif op in ('pushw',):
            depth += 2
        elif op in ('pop', 'popq', 'popf', 'popfq'):
            if args == '%rsp':
                errors.append(f'{pc:x}: unknown RSP pop')
                continue
            depth -= 8
            if args == '%rbp': rbp = None
            if args == '%r15': r15 = None
        elif op in ('leave', 'leaveq'):
            if rbp is None:
                errors.append(f'{pc:x}: unknown frame-pointer restore')
                continue
            depth, rbp = rbp - 8, None
        elif stack_write(op, args):
            delta = re.fullmatch(r'\$(0x[0-9a-f]+|[0-9]+),%rsp', args)
            lea = re.fullmatch(r'(-?0x[0-9a-f]+)?\(%(rsp|rbp|r15)\),%rsp', args)
            if op in ('sub', 'add') and delta:
                amount = signed(int(delta[1], 0))
                depth += amount if op == 'sub' else -amount
            elif op == 'and' and delta:
                mask = int(delta[1], 0)
                alignment = ((~mask) & ((1 << 64) - 1)) + 1
                if alignment == 0 or alignment & (alignment - 1):
                    errors.append(f'{pc:x}: non-alignment RSP mask')
                    continue
                depth += alignment - 1
            elif op == 'mov' and args in ('%rbp,%rsp', '%r15,%rsp'):
                saved = rbp if args.startswith('%rbp') else r15
                if saved is None:
                    errors.append(f'{pc:x}: unknown saved RSP')
                    continue
                depth = saved
            elif op == 'lea' and lea:
                saved = {'rsp': depth, 'rbp': rbp, 'r15': r15}[lea[2]]
                if saved is None:
                    errors.append(f'{pc:x}: unknown LEA stack base')
                    continue
                depth = saved - int(lea[1] or '0', 0)
            else:
                errors.append(f'{pc:x}: unknown RSP update: {op} {args}')
                continue
        elif op == 'mov' and args == '%rsp,%rbp': rbp = depth
        elif op == 'mov' and args == '%rsp,%r15': r15 = depth
        else:
            # Forget overwritten saved stack pointers; never infer restoration
            # from arbitrary register contents. RBP/R15 are callee-preserved.
            destination = args.rsplit(',', 1)[-1]
            if op not in ('cmp', 'test'):
                if destination in ('%rbp', '%ebp', '%bp', '%bpl'): rbp = None
                if destination in ('%r15', '%r15d', '%r15w', '%r15b'): r15 = None
        maximum = max(maximum, depth, before + 8 if op.startswith('call') else before)
        if maximum > ALL_LIMIT:
            errors.append(f'{pc:x}: frame exceeds {ALL_LIMIT} bytes (or growing loop)')
            break
        if depth < 0:
            errors.append(f'{pc:x}: stack underflow')
            continue
        targets = []
        if op.startswith(('ret', 'iret', 'sysret')) or op in ('ud2', '(bad)'):
            continue
        if op.startswith('call'):
            if direct_target(args) is None: indirect.append(pc)
        if op.startswith('j') or op.startswith('loop'):
            destination = direct_target(args)
            if destination is not None:
                if destination in code: targets.append(destination)
                elif destination not in owner: errors.append(f'{pc:x}: unresolved direct branch')
            elif op == 'jmp':
                table = re.fullmatch(r'\*(-?0x[0-9a-f]+)\(,%r\w+,8\)', args)
                if table:
                    address = int(table[1], 0) % (1 << 64)
                    data = next((data[address - base:] for base, data, execute in sections if isinstance(data, bytes) and not execute and base <= address < base + len(data)), b'')
                    # The next referenced table bounds this table. LLVM may
                    # fill impossible enum cases with nonlocal/invalid values:
                    # do not stop there and miss later valid cases. Include
                    # every local code pointer up to the next table/section
                    # end, a conservative superset rather than a guessed size.
                    end = (table_ends or {}).get(address)
                    if end is not None: data = data[:end - address]
                    for offset in range(0, len(data) - 7, 8):
                        target = struct.unpack_from('<Q', data, offset)[0]
                        if target in code: targets.append(target)
                    if not targets: errors.append(f'{pc:x}: unresolved jump table')
                elif depth == initial:
                    indirect.append(pc)  # restored frame: indirect tail call
                else:
                    errors.append(f'{pc:x}: unresolved non-tail indirect jump')
            else: errors.append(f'{pc:x}: unresolved conditional branch')
            if op == 'jmp':
                for target in set(targets): pending.append((target, depth, rbp, r15))
                continue
        if pc in next_pc: targets.append(next_pc[pc])
        for target in set(targets): pending.append((target, depth, rbp, r15))
    # Dead code is not a license for an unmodelled stack manipulation. In
    # particular a missed indirect local destination cannot hide a large frame.
    for pc, op, args in body:
        if pc not in visited and (stack_write(op, args) or op.startswith(('push', 'pop'))):
            errors.append(f'{pc:x}: unchecked stack instruction in unreachable block')
    return maximum, errors, sorted(set(indirect))


def audit(bodies, names, sections=(), contracts=None, roots=ROOTS, boot_roots=BOOT_ROOTS):
    contracts = contracts or {}
    owner = {pc: start for start, body in bodies.items() for pc, _, _ in body}
    by_start = {start: name for name, start in names.items()}
    normalized = {plain(name): start for name, start in names.items()}
    errors = []
    def closure(wanted):
        pending, visited = [], set()
        for name in wanted:
            if name not in normalized: errors.append(f'missing root: {name}')
            else: pending.append(normalized[name])
        while pending:
            start = pending.pop()
            if start in visited: continue
            visited.add(start)
            for pc, op, args in bodies[start]:
                if op.startswith(('call', 'j', 'loop')):
                    target = direct_target(args)
                    if target is not None:
                        if target not in owner: errors.append(f'{pc:x}: unresolved direct transfer')
                        elif owner[target] != start: pending.append(owner[target])
        return visited
    runtime_roots = tuple(roots) + tuple(n for n in names if n.startswith('traps.SyscallEntryForCpu('))
    runtime, boot = closure(runtime_roots), closure(boot_roots)
    tables = sorted({int(match[1], 0) % (1 << 64)
                     for body in bodies.values() for _, op, args in body
                     if op == 'jmp' and (match := re.fullmatch(r'\*(-?0x[0-9a-f]+)\(,%r\w+,8\)', args))})
    table_ends = dict(zip(tables, tables[1:]))
    rows = []
    for start, body in bodies.items():
        name = by_start[start]
        maximum, failures, indirect = frame(plain(name), body, owner, sections, contracts.get(plain(name)), table_ends)
        runtime_reachable = start in runtime
        boot_only = start in boot and not runtime_reachable
        limit = RUNTIME_LIMIT if runtime_reachable else ALL_LIMIT
        # The stricter bound follows only the requested direct-call closure;
        # indirect callees are reported, not promoted by an invented analysis.
        if maximum is not None and maximum > limit: failures.append(f'frame {maximum} exceeds {limit}')
        if runtime_reachable and (plain(name) in (*boot_roots, 'iommu.init', 'boot.entry.bootWithResources', 'boot.entry.persistBootResourcesOrHalt') or name in VERIFIED_AGGREGATE):
            failures.append('boot-only/verified aggregate function reached from runtime root')
        errors.extend(name + ': ' + failure for failure in failures)
        rows.append(dict(symbol=name, maximum_bytes=maximum, limit_bytes=limit, runtime_reachable=runtime_reachable, boot_only=boot_only, indirect_transfers=[hex(pc) for pc in indirect]))
    return dict(errors=errors, functions=rows, roots=list(runtime_roots), boot_roots=list(boot_roots))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    sections = elf_sections(args.elf)
    assembly = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(args.elf)], text=True)
    contracts = json.loads(Path(__file__).with_name('kernel_stack_switch_contracts.json').read_text())
    report = audit(*parse_disassembly(assembly), sections, contracts)
    report.update(elf=str(args.elf), sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(), scope=__doc__)
    if args.output: args.output.write_text(json.dumps(report, indent=2) + '\n')
    for row in sorted(report['functions'], key=lambda r: r['maximum_bytes'] or 0, reverse=True)[:20]:
        print(f"{row['maximum_bytes']:>7} / {row['limit_bytes']:>6}  {'runtime' if row['runtime_reachable'] else 'boot' if row['boot_only'] else 'unreached':9} {row['symbol']}")
    if report['errors']: raise SystemExit('\n'.join(report['errors']))
    print(f"Kernel stack frames: {len(report['functions'])} functions PASS")


if __name__ == '__main__':
    main()
