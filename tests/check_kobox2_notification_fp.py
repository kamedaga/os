#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check emitted notification entry/return code, not only C attributes."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

GPR = set('endbr64 push pop mov movabs lea xor test cmp xchg add sub and or '
          'je jne jz jnz jmp ja jae jb jbe call ret nop nopl nopw cs data16 ud2 '
          'lock imul movslq movzbl movzwl movb movl movw cmpb cmpl cmpq '
          'testb testl addl inc incl incq dec decl sbb neg not sar shr shl sal sete setne seta cmove cmovne'.split())
# The clock page reader converts KVM pvclock with 128-bit integer arithmetic
# and an lfence-ordered rdtsc. None of these touch x87/MMX/SSE/AVX state.
GPR |= set('lfence rdtsc mul shld shrd js jl jg cmova cmovb movsbl'.split())
# A constant 64-bit integer memory store is printed as movq. MMX/SSE movq
# still fails the operand register check below; the suffix alone is not SIMD.
GPR |= {'movq', 'xadd'}


def symbol_copies(path, symbol):
    names = subprocess.check_output(['nm', '-S', '--defined-only', str(path)], text=True)
    return [(int(start, 16), int(size, 16)) for start, size in re.findall(
        r'^([0-9a-f]+) ([0-9a-f]+) \S ' + re.escape(symbol) + r'$', names, re.M)]


def instruction_copies(path, symbol):
    # Static helpers with the same name occur in several TUs. GNU objdump's
    # --disassemble=name selects only one; inspect every address/size pair.
    rows = []
    for start, size in symbol_copies(path, symbol):
        text = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn',
            '--start-address=' + hex(start), '--stop-address=' + hex(start + size),
            str(path)], text=True)
        rows.append(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+(\S+)[ \t]*(.*)$', text, re.M))
    assert rows, (path, symbol, 'missing symbol')
    return rows


def instructions(path, symbol):
    return [row for copy in instruction_copies(path, symbol) for row in copy]


def leaf(path, symbol, allowed_calls, getter_call=False, syscall_leaf=False):
    copies = instruction_copies(path, symbol)
    for rows in copies:
        indirect = 0
        for op, operand in rows:
            assert (op in GPR or (syscall_leaf and op == 'syscall')) and not re.search(r'%(?:[xyz]mm|mm\d|st)', operand), (path, symbol, op, operand)
            if op == 'call' or op.startswith('j'):
                target = re.search(r'<([^>]+)>', operand)
                if not target and getter_call and op == 'call' and operand.startswith('*%'):
                    # Verify one borrowed getter in EACH emitted copy.
                    indirect += 1
                    continue
                assert target, (path, symbol, 'indirect transfer', operand)
                callee = target[1].split('+')[0]
                assert callee == symbol or callee in allowed_calls, (path, symbol, callee)
        if getter_call:
            assert indirect == 1, (path, symbol, indirect)
        if syscall_leaf:
            assert sum(op == 'syscall' for op, operand in rows) == 1, (path, symbol, 'syscall count')
    return [row for copy in copies for row in copy]


def single_tls_access(path, symbol, rows, require=True):
    # Unmasked code can migrate to another native thread at any instruction.
    # Thread words must be one %fs-relative instruction each; loading the
    # FS:0 self pointer and dereferencing it later would name the old thread.
    fs = [operand for op, operand in rows if '%fs:' in operand]
    assert fs or not require, (path, symbol, 'no %fs-relative access')
    assert not any(re.search(r'%fs:0x0\b|%fs:0\b', operand) for operand in fs), (path, symbol, fs)
    return fs


def core_getter(path):
    data = path.read_bytes()
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    assert header[0][:6] == b'\x7fELF\x02\x01' and header[2] == 62
    segments = [struct.unpack_from('<IIQQQQQQ', data, header[5] + i * header[9])
                for i in range(header[10])]

    def fixed_pointer(address):
        for segment in segments:
            if segment[0] == 1 and segment[3] <= address and address + 8 <= segment[3] + segment[5]:
                return struct.unpack_from('<Q', data, segment[2] + address - segment[3])[0]
        raise AssertionError((path, 'GOT address outside file-backed LOAD', hex(address)))

    tls = symbol_copies(path, 'kobox_runtime_thread_state')
    report = {'path': str(path), 'entries': {}, 'getter_got_targets': []}
    for symbol in ('port_cpu', 'kobox_runtime_thread_state'):
        copies = instruction_copies(path, symbol)
        for rows in copies:
            calls = []
            for op, operand in rows:
                assert op in GPR and not re.search(r'%(?:[xyz]mm|mm\d|st)', operand), (path, symbol, op, operand)
                if op == 'call':
                    calls.append(operand)
                elif op.startswith('j'):
                    target = re.search(r'<([^>]+)>', operand)
                    assert target and target[1].split('+')[0] == symbol, (path, symbol, operand)
            assert len(calls) == 1, (path, symbol, calls)
            if symbol == 'port_cpu':
                # Hosted large-model code resolves this one GOT call to the
                # audited runtime helper. Do not accept arbitrary indirect C.
                base = [int(m[1], 16) for op, operand in rows if op == 'lea'
                        and (m := re.search(r'# ([0-9a-f]+) <port_cpu', operand))]
                constants = [int(m[1], 16) for op, operand in rows if op == 'movabs'
                             and (m := re.search(r'\$0x([0-9a-f]+),%', operand))]
                assert len(base) == 1 and len(constants) == 2, (path, rows)
                assert re.fullmatch(r'\*\(%\w+,%\w+,1\)', calls[0]), (path, calls)
                got = (base[0] + sum(constants)) & ((1 << 64) - 1)
                target = fixed_pointer(got)
                assert target in {start for start, size in tls}, (path, hex(got), hex(target), tls)
                report['getter_got_targets'].append({'got': hex(got), 'target': hex(target)})
            else:
                # This single register-indirect call is the bound thread_state
                # leaf. Audit its concrete native/POSIX entries separately.
                assert re.fullmatch(r'\*%\w+', calls[0]), (path, calls)
        report['entries'][symbol] = {'copies': symbol_copies(path, symbol), 'instructions': copies}
    return report


def preserving(path, symbol, body):
    rows = instructions(path, symbol)
    if rows[0][0] == 'endbr64':
        rows = rows[1:]
    # GCC may emit unreachable UD2/padding after the naked RET. The live
    # return sequence must have no compiler epilogue following FXRSTOR.
    live = rows[:next(i for i, row in enumerate(rows) if row[0] == 'ret') + 1]
    assert [r[0] for r in live] == ['sub', 'fxsave64', 'fxrstor64', 'call', 'fxrstor64', 'add', 'ret'], (path, symbol, live)
    assert live[0][1] == '$0x208,%rsp' and live[5][1] == '$0x208,%rsp'
    assert live[1][1] == '(%rsp)' and live[4][1] == '(%rsp)'
    assert '<kobox_fp_entry_initial>' in live[2][1] and '<' + body + '>' in live[3][1]
    return live


for argument in sys.argv[1:]:
    path = Path(argument)
    names = subprocess.check_output(['nm', '--defined-only', str(path)], text=True)
    if re.search(r'\bport_cpu$', names, re.M):
        print(json.dumps(core_getter(path), sort_keys=True))
        continue
    report = {'path': str(path), 'entries': {}, 'symbol_copies': {}}
    if re.search(r'\b(?:monotonic|mono_call)$', names, re.M) and not re.search(r'\bkobox_posix_monotonic_ns$', names, re.M):
        specialized = set(re.findall(r'\b(clock_ns\.[\w.]+)$', names, re.M))
        clock_allowed = specialized | {'clock_ns', 'pacha_clock_read', 'pacha_syscall2', 'clock_mock', 'ph_tail_begin', 'ph_tail_end'}
        for symbol in ('ph_tail_begin', 'ph_tail_end'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, {'ph_current_cpu', 'ph_current_fiber'})
        for symbol in ('monotonic', 'realtime', 'clock_ns', 'mono_call', 'real_call', 'invalid_call', 'pacha_clock_read', 'pacha_syscall2', 'clock_mock'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, clock_allowed | {'ph_notifications_dispatch_preserved'}, syscall_leaf=symbol in ('pacha_clock_read', 'pacha_syscall2'))
        # Specialized static copies are part of the same ABI callgraph.
        for symbol in specialized:
            report['entries'][symbol] = leaf(path, symbol, clock_allowed)
    for name, body in (('kobox_posix_monotonic_ns', 'monotonic_body'),
                       ('kobox_posix_realtime_ns', 'realtime_body')):
        if re.search(r'\b' + name + r'$', names, re.M):
            report['entries'][name] = preserving(path, name, body)
    if re.search(r'\bph_notifications_dispatch_preserved$', names, re.M):
        helpers = {symbol for symbol in ('ph_current_task', 'test_current_task', 'ph_test_current_task')
                   if re.search(r'\b' + symbol + r'$', names, re.M)}
        # The native getter is always inlined; only test builds name a helper.
        native_tls = not (helpers & {'test_current_task', 'ph_test_current_task'})
        # The fiber fixtures use the named test hook declared by host.h. Audit
        # its emitted body too, rather than permitting an unchecked getter.
        for helper in helpers:
            report['entries'][helper] = leaf(path, helper, helpers)
        for symbol in ('ph_current_fiber', 'ph_current_cpu'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, helpers | {'ph_current_fiber'})
        allowed = helpers | {'ph_fail', 'ph_notifications_dispatch_preserved'}
        for symbol in ('ph_notifications_save', 'ph_notifications_restore'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, allowed)
                if native_tls:
                    single_tls_access(path, symbol, report['entries'][symbol])
            else:
                # Compilers can inline primitives into address-taken test
                # wrappers; audit those emitted entry/return paths instead.
                wrapper = 'save_call' if symbol.endswith('save') else 'restore_call'
                report['entries'][wrapper] = leaf(path, wrapper, allowed)
        # The handler and dispatcher run unmasked and may migrate. They need
        # not touch thread words directly, but must never hold the FS:0
        # pointer. Scheduler-diagnostic builds snapshot it on purpose.
        if native_tls and not re.search(r'\bdiagnostic_snapshot$', names, re.M):
            for symbol in ('ph_notification_body', 'ph_dispatch_notifications'):
                if re.search(r'\b' + symbol + r'$', names, re.M):
                    single_tls_access(path, symbol, instructions(path, symbol), require=False)
        report['entries']['ph_notifications_dispatch_preserved'] = preserving(
            path, 'ph_notifications_dispatch_preserved', 'ph_notifications_dispatch_body')
        # IRQ entries use only GPR code before the shared protected dispatch.
        # Audit all emitted copies of the TLS helper, not just attributes.
        irq_allowed = helpers | {'ph_current_task', 'test_current_task', 'ph_current_fiber', 'ph_current_cpu', 'ph_fail',
                       'kobox_machine_domain_irq_disable', 'kobox_machine_domain_irq_enable',
                       'kobox_machine_domain_pending',
                       'ph_notifications_dispatch_preserved', 'ph_notifications_save',
                       'ph_notifications_restore', 'irq_disable', 'irq_enable', 'irq_disabled'}
        for symbol in ('irq_disable', 'irq_enable', 'irq_disabled', 'thread_state',
                       'kobox_machine_domain_irq_disable', 'kobox_machine_domain_irq_enable',
                       'kobox_machine_domain_pending'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, irq_allowed)
                report['symbol_copies'][symbol] = symbol_copies(path, symbol)
                if symbol == 'thread_state' and native_tls:
                    # It may call the out-of-line fiber getter; audit both.
                    rows = list(report['entries'][symbol])
                    if any('<ph_current_fiber>' in operand for op, operand in rows):
                        rows += report['entries']['ph_current_fiber']
                    single_tls_access(path, symbol, rows)
        if re.search(r'\birq_transaction$', names, re.M):
            report['entries']['irq_transaction'] = leaf(path, 'irq_transaction', irq_allowed, getter_call=True)
            report['symbol_copies']['irq_transaction'] = symbol_copies(path, 'irq_transaction')
        if re.search(r'\bph_current_task$', names, re.M):
            report['symbol_copies']['ph_current_task'] = symbol_copies(path, 'ph_current_task')
        for symbol in ('query_call', 'disable_call', 'enable_call'):
            if re.search(r'\b' + symbol + r'$', names, re.M):
                report['entries'][symbol] = leaf(path, symbol, irq_allowed)
    elif re.search(r'\bkobox_posix_notifications_save$', names, re.M):
        for suffix in ('save', 'restore'):
            name = 'kobox_posix_notifications_' + suffix
            report['entries'][name] = preserving(path, name, 'notifications_' + suffix + '_body')
        for suffix in ('disable', 'enable', 'disabled'):
            name = 'host_cpu_irq_' + suffix
            if re.search(r'\b' + name + r'$', names, re.M):
                report['entries'][name] = preserving(path, name, name + '_body')
        if re.search(r'\bhost_cpu_irq_transaction$', names, re.M):
            name = 'host_cpu_irq_transaction'
            report['entries'][name] = preserving(path, name, name + '_body')
        if re.search(r'\bthread_state$', names, re.M):
            report['entries']['thread_state'] = leaf(path, 'thread_state', set())
            report['symbol_copies']['thread_state'] = symbol_copies(path, 'thread_state')
    print(json.dumps(report, sort_keys=True))
