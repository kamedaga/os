#!/usr/bin/env python3
"""Tripwires for scheduler and lock ordering lock paths, not a whole-kernel lock proof."""
from pathlib import Path
import re
from check_kernel_address_boundaries import code_only, function_body

ROOT = Path(__file__).resolve().parents[1]
PATHS = ('scheduler_connection.zig', 'sync/lock_order.zig', 'traps.zig', 'sync/lock_diagnostics.zig', 'syscalls.zig', 'syscall/runtime.zig', 'memory/user_vm.zig', 'memory/free_page_list.zig', 'memory/address_space.zig', 'state/types.zig', 'memory/vmo_backing.zig', 'state/process.zig', 'user_copy.zig', 'vtd.zig', 'amd_iommu.zig', 'pci.zig', 'smp.zig', 'state/fd.zig', 'realtime_clock.zig', 'kernel_log.zig', 'boot/boot_diag.zig')


def check(sources):
    errors = []
    scheduler = code_only(sources[PATHS[0]])
    spin = scheduler[scheduler.index('const SchedulerSpinLock ='):scheduler.index('const CpuSchedulerState =')]
    acquire = function_body(spin, 'lock')
    release = function_body(spin, 'unlock')
    sequence = ('disableInterruptsForSchedulerLock()', 'lock_diagnostics.beforeAcquire(', '@cmpxchgWeak(', 'lock_diagnostics.acquired(')
    positions = [acquire.find(item) for item in sequence]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append('scheduler: IF/check/CAS/record publication order bypassed')
    if release.find('lock_diagnostics.releasing(') < 0 or release.find('lock_diagnostics.releasing(') > release.find('@atomicStore('):
        errors.append('scheduler: record must be detached before release store')
    if release.find('self.interrupts_were_enabled') < release.find('lock_diagnostics.releasing('):
        errors.append('scheduler: foreign unlock reads mutable IF snapshot before membership')
    if scheduler.count('@cmpxchgWeak(') != spin.count('@cmpxchgWeak('):
        errors.append('scheduler: new CAS outside the ranked common acquisition')
    if 'lock_diagnostics.assertAllQuiescent()' not in function_body(scheduler, 'initializeStaticStorage'):
        errors.append('scheduler: reset may erase linked records')
    keys = function_body(spin, 'diagnosticKey')
    leaf_identity = '&scheduler_state.thread_table.directory_lock'
    if (keys.count(leaf_identity) != 1 or
            'if (self == ' + leaf_identity + ') return .{ .class = .thread_directory };' not in keys):
        errors.append('scheduler: directory leaf must have exactly one terminal classification')
    if any(text not in keys for text in ('&scheduler_state.thread_table.lock_state', '&scheduler_state.thread_table.directory_lock', '.class = .thread_directory', '&state.lock', '&state.runqueue_lock', '.order = cpu', 'lock_diagnostics.failure(')):
        errors.append('scheduler: audited key identity / fail-closed classification bypassed')
    for name in ('currentPrincipal', 'currentCr3', 'applyPinnedThreadBases'):
        body = function_body(scheduler, name)
        if ('pinnedContextForCpu(cpu)' not in body or
                re.search(r'\b(?:threadContext\w*|threadDirectory|lockThreadDirectory)\s*\(', body) or
                '.lock(' in body):
            errors.append(name + ': current pin bypass / reentrant directory borrow')
    pick = function_body(scheduler, 'verifiedPickThreadForCpu')
    if re.sub(r'\s+', '', pick) != ('scheduler_state.thread_table.lock();'
            'deferscheduler_state.thread_table.unlock();returnverifiedPickThreadForCpuLocked(cpu_id);'):
        errors.append('pick: table wrapper bypass')
    claim = function_body(scheduler, 'claimKernelScheduledUserEntry')
    sequence = ('scheduler_state.thread_table.lock();', 'state.lock.lock();',
                'fillUserEntryForThread(', 'installCurrentContextLocked(')
    positions = [claim.find(item) for item in sequence]
    if min(positions) < 0 or positions != sorted(positions) or 'defer scheduler_state.thread_table.unlock();' not in claim:
        errors.append('AP claim: table/CPU validation/pin order bypass')
    precheck = function_body(code_only(sources[PATHS[1]]), 'beforeAcquire')
    if re.search(r'\brecord\.\w+', precheck):
        errors.append('checker: before CAS reads attempted record mutable metadata')
    diagnostic = code_only(sources[PATHS[3]])
    failure = function_body(diagnostic, 'failure')
    for name in re.findall(r'^var\s+(\w+)\b', diagnostic, re.M):
        if re.search(r'\b' + re.escape(name) + r'\s*=', failure):
            errors.append('diagnostic: unsynchronized global fault publication')
    traps = code_only(sources[PATHS[2]])
    for name in ('timerInterruptDispatch', 'deviceInterruptDispatch', 'schedulerMaintenanceIpiDispatch', 'schedulerWakeIpiDispatch'):
        body = function_body(traps, name)
        if body.count('lock_diagnostics.enterInterrupt(') != 1 or body.count('defer diagnostic_scope.end()') != 1:
            errors.append(name + ': IRQ context pairing bypassed')
        for match in re.finditer(r'smp\.returnCurrentAp\w*\(', body):
            prefix = body[:match.start()]
            if not re.search(r'lock_diagnostics\.assertRootEmpty\(\);\s*$', prefix):
                errors.append(name + ': noreturn AP transition abandons unchecked holds')
    syscalls = code_only(sources['syscalls.zig'])
    syscalls_raw = sources['syscalls.zig']
    compact = lambda code: re.sub(r'\s+', '', code)
    admission = compact(function_body(syscalls, 'interruptCanCompleteUserWait'))
    if admission != 'returnfrom_userorscheduler.currentCpuInLockFreeIdleWait();':
        errors.append('IRQ copyout: shared user/lock-free idle admission bypassed')
    for name in ('completePendingIrqFdWaitersFromInterrupt',
                 'completePendingTimerFdWaitersFromInterrupt',
                 'completeProcessSignalTimersFromInterrupt'):
        body = compact(function_body(syscalls, name))
        gate = body.find('!interruptCanCompleteUserWait(from_user)')
        acquire = body.find('kernel_state_lock.tryLock()')
        if gate < 0 or acquire < 0 or gate > acquire:
            errors.append(name + ': unsafe IRQ must return before acquiring K or consuming waiters')
        if name == 'completePendingTimerFdWaitersFromInterrupt':
            if ('if(!interruptCanCompleteUserWait(from_user)oruser_copy.tlbShootdownActiveOnCurrentCpu())'
                    '{clockevent.deferContendedDelivery(now_ns);return;}') not in body:
                errors.append('timer FD: unsafe IRQ must preserve deadline and schedule retry')
        elif 'if(!interruptCanCompleteUserWait(from_user))return;' not in body:
            errors.append(name + ': unsafe IRQ must leave completion pending')
    for admission_name, worker_name in (
        ('completePendingIrqFdWaitersFromInterrupt', 'completePendingIrqFdWaitersLocked'),
        ('completePendingTimerFdWaitersFromInterrupt', 'completePendingTimerFdWaitersLocked'),
    ):
        body = compact(function_body(syscalls, admission_name))
        worker = compact(function_body(syscalls, worker_name))
        if ('ThreadWakeTarget' in body or body.find(worker_name + '(') < body.find('deferkernel_state_lock.unlock();')
                or body.count(worker_name + '(') != 1
                or not re.search(r'noinline\s+fn\s+' + worker_name + r'\(', syscalls)
                or 'ThreadWakeTarget' not in worker):
            errors.append(admission_name + ': snapshot must remain in a noninline worker under K')
    timer = compact(function_body(traps, 'timerInterruptDispatch'))
    for call in ('completePendingTimerFdWaitersFromInterrupt(now_nsorelsenext_tick*|1_000_000,user_mode)',
                 'completeProcessSignalTimersFromInterrupt(scheduler.lapic_tick_count,user_mode)',
                 'completePendingIrqFdWaitersFromInterrupt(user_mode)'):
        if timer.count(call) != 1:
            errors.append('timer IRQ: interrupted user mode must reach every copyout admission')
    device = compact(function_body(traps, 'deviceInterruptDispatch'))
    if device.count('completeDeviceIrqFdWaitersFromInterrupt((frame.cs&3)==3and(frame.ss&3)==3)') != 1:
        errors.append('device IRQ: interrupted privilege must reach copyout admission')
    # The device entry publishes the pending hint before probing K (Dekker with
    # the owner's post-release fence), then takes the ordinary admission path.
    wrapper = compact(function_body(syscalls, 'completeDeviceIrqFdWaitersFromInterrupt'))
    if wrapper != ('@atomicStore(bool,&irq_fd_completion_pending,true,.seq_cst);'
                   'completePendingIrqFdWaitersFromInterrupt(from_user);'):
        errors.append('device IRQ: pending hint must be published before the K probe')
    # Handoff runs only after an eligible IRQ released K, never ahead of the
    # privilege gate, and rechecks the shootdown guard before each tryLock.
    admission = compact(function_body(syscalls, 'completePendingIrqFdWaitersFromInterrupt'))
    if (not admission.endswith('kernel_state_lock.unlock();handOffIrqFdWaitersAfterRelease();')
            or admission.count('handOffIrqFdWaitersAfterRelease(') != 1):
        errors.append('IRQ FD admission: handoff must follow its own K release')
    handoff = compact(function_body(syscalls, 'handOffIrqFdWaitersAfterRelease'))
    # code_only drops string literals; bind the barrier's mnemonic on raw text.
    if compact(function_body(syscalls_raw, 'handOffIrqFdWaitersAfterRelease')).count('asmvolatile("mfence":::.{.memory=true});') != 1:
        errors.append('IRQ FD handoff: release->hint store-load barrier missing')
    order = [handoff.find(token) for token in (
        'asmvolatile(:::.{.memory=true});',
        '@atomicLoad(bool,&irq_fd_completion_pending,.monotonic)',
        'user_copy.tlbShootdownActiveOnCurrentCpu()',
        'kernel_state_lock.tryLock()',
        'completePendingIrqFdWaitersLocked(h);',
        'kernel_state_lock.unlock();')]
    if min(order) < 0 or order != sorted(order):
        errors.append('IRQ FD handoff: fence, hint, shootdown guard and K must stay ordered')
    worker = compact(function_body(syscalls, 'completePendingIrqFdWaitersLocked'))
    if not worker.startswith('_=@atomicRmw(bool,&irq_fd_completion_pending,.Xchg,false,.acquire);'):
        errors.append('IRQ FD worker: pending hint must be consumed before the scan')
    # Each audited primitive must serialize CAS + intrusive publication with
    # local IRQs. These are actual method bodies, not test-only model locks.
    for path, marker, class_name in (
        ('syscalls.zig', 'const KernelStateSpinLock =', 'kernel_state'),
        ('syscall/runtime.zig', 'const FutexSpinLock =', 'futex'),
        ('memory/vmo_backing.zig', 'pub const Lock =', 'vmo_backing'),
        ('memory/free_page_list.zig', 'pub fn WithQuarantine', 'pmm'),
        ('user_copy.zig', 'const TlbShootdownLock =', 'tlb_shootdown'),
        ('vtd.zig', 'fn lock()', 'vtd'),
        ('amd_iommu.zig', 'fn lock()', 'amd_iommu'),
    ):
        text = code_only(sources[path]); text = text[text.index(marker):]
        acquire = function_body(text, 'lock'); release = function_body(text, 'unlock')
        seq = ('lock_diagnostics.maskInterrupts()', 'lock_diagnostics.beforeAcquire(', '@cmpxchgWeak(', 'lock_diagnostics.acquired(', 'lock_diagnostics.restoreInterrupts(')
        positions = [acquire.find(x) for x in seq]
        if min(positions) < 0 or positions != sorted(positions) or '.class = .' + class_name not in acquire:
            errors.append(path + ': lock ordering acquisition mask/check/CAS/link/restore bypass')
        if (release.find('lock_diagnostics.releasing(') < 0 or
                release.find('lock_diagnostics.releasing(') > release.find('@atomicStore(') or
                'lock_diagnostics.maskInterrupts()' not in release):
            errors.append(path + ': lock ordering release mask/unlink/store bypass')
    for path, acquire_name, release_name in (
        ('memory/address_space.zig', 'lockDirectory', 'unlockDirectory'),
        ('state/process.zig', 'lockDescriptors', 'unlockDescriptors'),
        ('state/process.zig', 'lockVmaDirectory', 'unlockVmaDirectory'),
        ('pci.zig', 'lockConfig', 'unlockConfig'),
    ):
        text = code_only(sources[path]); acquire = function_body(text, acquire_name); release = function_body(text, release_name)
        positions = [acquire.find(x) for x in ('lock_diagnostics.beforeAcquire(', '@cmpxchgWeak(', 'lock_diagnostics.acquired(')]
        if min(positions) < 0 or positions != sorted(positions): errors.append(path + ': lock ordering leaf acquisition bypass')
        if release.find('lock_diagnostics.releasing(') < 0 or release.find('lock_diagnostics.releasing(') > release.find('@atomicStore('):
            errors.append(path + ': lock ordering leaf release bypass')
    for path, name in (('syscalls.zig', 'tryLock'), ('state/process.zig', 'tryProcessLabel')):
        body = function_body(code_only(sources[path]), name)
        cas = body.find('@cmpxchgStrong(')
        positions = [cas, body.find('return ', cas), body.find('lock_diagnostics.acquiredTry(')]
        if min(positions) < 0 or positions != sorted(positions): errors.append(path + ': try failure/success recording bypass')
    vm = code_only(sources['memory/user_vm.zig']); acquire = function_body(vm, 'lockState'); release = function_body(vm, 'unlockState')
    if any(x not in acquire for x in ('lock_diagnostics.maskInterrupts()', 'lock_diagnostics.recursive(', 'error.InvalidRecursion', 'error.DepthOverflow', 'lock_diagnostics.beforeAcquire(', 'lock_diagnostics.acquired(')):
        errors.append('AS/shared: recursive context or first-link checks bypassed')
    if any(x not in release for x in ('lock_diagnostics.assertOwned(', 'error.DepthUnderflow', 'lock_diagnostics.releasing(')):
        errors.append('AS/shared: last unlink ownership bypassed')
    order = code_only(sources['sync/lock_order.zig'])
    recursion = function_body(order, 'recursive')
    if 'node.context != self.interrupt_depth' not in recursion or 'error.WrongContext' not in recursion:
        errors.append('AS/shared: IRQ reentry admitted')
    ipi = function_body(order, 'assertCanWaitForIpi')
    if 'node.key.class.forbidsIpiWait()' not in ipi or 'node.context' in ipi or 'node.mode' in ipi:
        errors.append('IPI: all interrupted/try holds must be checked')
    for path, name in (('user_copy.zig', 'shootdownRequest'), ('smp.zig', 'drainInterruptRange'), ('scheduler_connection.zig', 'waitForPrincipalQuiescenceIgnoring')):
        body = function_body(code_only(sources[path]), name)
        if body.count('lock_diagnostics.assertCanWaitForIpi();') != 1:
            errors.append(path + ': IPI wait admission bypassed')
    window = code_only(sources['user_copy.zig']); window = window[window.index('const PhysCopyWindowGuard'):window.index('fn boundedCurrentCpuSlot')]
    if any(x not in window for x in ('lock_diagnostics.beforeAcquire(', 'lock_diagnostics.acquired(', 'lock_diagnostics.assertOwned(', 'lock_diagnostics.releasing(', 'error.ContextMovedCpu')):
        errors.append('copy window: per CPU ownership bypassed')
    lifecycle = function_body(scheduler, 'tryBeginPrincipalLifecycleGate')
    if any(token not in lifecycle for token in ('lock_diagnostics.maskInterrupts()', 'lock_diagnostics.acquiredTry(&principal_lifecycle_record', '.class = .principal_lifecycle')):
        errors.append('L: successful nonwaiting acquisition must be recorded')
    end = function_body(scheduler, 'endPrincipalLifecycle')
    if end.find('lock_diagnostics.releasing(') < 0 or end.find('lock_diagnostics.releasing(') > end.find('@atomicStore(usize, &principal_lifecycle_gate, 0'):
        errors.append('L: unlink before gate publication required')
    q = function_body(code_only(sources['state/fd.zig']), 'recordDeviceInterruptEvent')
    positions = [q.find(x) for x in ('while (true)', 'self.object_directory_leaf.lock()', 'self.irq_publish_slots.get(', '@atomicRmw(', 'const owner_raw = slot.owner_principal_raw;')]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append('Q: each candidate must acquire directory leaf before lookup and event addition')
    tail = q[q.find('const owner_raw = slot.owner_principal_raw;'):]
    if not 0 <= tail.find('self.object_directory_leaf.unlock(flags)') < tail.find('appendUniquePrincipal') or 'slot.' in tail[tail.find('self.object_directory_leaf.unlock(flags)'):]:
        errors.append('Q: release before completion; only owner scalar may escape')
    if any(token in q for token in ('kernel_state_lock', 'beginExternalPrincipalTeardown', '.allocate(', 'write_user', '@cmpxchgWeak')):
        errors.append('Q: leaf forbids external locks, allocations, copyout and old handshake')
    for publication in ('publishIrqObject', 'unpublishIrqObject'):
        unpublish = function_body(code_only(sources['state/fd.zig']), publication)
        if unpublish.find('self.object_directory_leaf.lock()') < 0 or unpublish.find('self.object_directory_leaf.lock()') > unpublish.find('self.irqPublishSlotForRef(') or 'defer self.object_directory_leaf.unlock(flags)' not in unpublish:
            errors.append('Q: publication/withdrawal requires directory leaf')
    clock = code_only(sources['realtime_clock.zig'])
    for name in ('tryPublishPv', 'beginFallback', 'enable'):
        body = function_body(clock, name)
        mode = 'acquired(' if name == 'enable' else 'acquiredTry('
        if 'lock_diagnostics.' + mode not in body or 'lock_diagnostics.releasing(' not in body or 'lock_diagnostics.maskInterrupts()' not in body:
            errors.append('clock: acquisition mode or release bypassed')
        if name == 'enable' and not 0 <= body.find('lock_diagnostics.beforeAcquire(') < body.find('while ('):
            errors.append('clock: blocking wait must be checked first')
    ring = code_only(sources['kernel_log.zig'])
    if 'self.tryLock(.blocking)' not in function_body(ring, 'snapshot') or 'self.tryLock(.try_once)' not in function_body(ring, 'append'):
        errors.append('log: bounded snapshot is waiting, writer is one-shot')
    take = function_body(ring, 'tryLock')
    if not 0 <= take.find('lock_diagnostics.beforeAcquire(') < take.find('@cmpxchgStrong('):
        errors.append('log: rank check must precede first snapshot attempt')
    if 'lock_diagnostics.acquiredTry(' not in take or 'lock_diagnostics.acquired(' not in take or 'lock_diagnostics.releasing(' not in function_body(ring, 'unlock'):
        errors.append('log: hold recording bypassed')
    boot = code_only(sources['boot/boot_diag.zig'])
    for name, record in (('begin','drawing_record'), ('traceText','trace_record')):
        body = function_body(boot, name)
        if 'lock_diagnostics.acquiredTry(&' + record not in body or 'lock_diagnostics.maskInterrupts()' not in body:
            errors.append('boot: exchange try publication bypassed')
    if 'lock_diagnostics.releasing(&drawing_record)' not in function_body(boot, 'finish') or 'lock_diagnostics.releasing(&trace_record)' not in function_body(boot, 'traceText'):
        errors.append('boot: record release bypassed')
    return errors


def main():
    sources = {path: (ROOT / 'kernel/src' / path).read_text() for path in PATHS}
    failures = check(sources)
    if failures:
        raise SystemExit('\n'.join(failures))
    mutations = (
        ('state/fd.zig', 'const flags = self.object_directory_leaf.lock();', 'const flags = false;'),
        ('state/fd.zig', 'const owner_raw = slot.owner_principal_raw;', 'const owner_raw = 0;'),
        ('state/fd.zig', 'self.object_directory_leaf.unlock(flags);\n        const owner =', 'const owner ='),
        ('syscalls.zig', 'noinline fn completePendingIrqFdWaitersLocked', 'fn completePendingIrqFdWaitersLocked'),
        ('syscalls.zig', 'noinline fn completePendingTimerFdWaitersLocked', 'fn completePendingTimerFdWaitersLocked'),
        ('syscalls.zig', 'completePendingIrqFdWaitersLocked(h);', 'bypassWorker(h);'),

        ('syscalls.zig', 'return from_user or scheduler.currentCpuInLockFreeIdleWait();', 'return true;'),
        ('syscalls.zig', 'if (!interruptCanCompleteUserWait(from_user)) return;', ''),
        ('syscalls.zig', '!interruptCanCompleteUserWait(from_user) or ', ''),
        ('traps.zig', 'completePendingIrqFdWaitersFromInterrupt(user_mode)', 'completePendingIrqFdWaitersFromInterrupt(true)'),
        ('traps.zig', 'completeDeviceIrqFdWaitersFromInterrupt((frame.cs & 3) == 3 and (frame.ss & 3) == 3)', 'completeDeviceIrqFdWaitersFromInterrupt(true)'),
        ('traps.zig', 'completeDeviceIrqFdWaitersFromInterrupt((frame.cs & 3) == 3 and (frame.ss & 3) == 3)', 'completePendingIrqFdWaitersFromInterrupt((frame.cs & 3) == 3 and (frame.ss & 3) == 3)'),
        ('syscalls.zig', '    @atomicStore(bool, &irq_fd_completion_pending, true, .seq_cst);\n    completePendingIrqFdWaitersFromInterrupt(from_user);',
         '    completePendingIrqFdWaitersFromInterrupt(from_user);\n    @atomicStore(bool, &irq_fd_completion_pending, true, .seq_cst);'),
        ('syscalls.zig', '    kernel_state_lock.unlock();\n    // This context passed the copyout checks above, so it can also take over\n    // a device IRQ that lost tryLock to this hold.\n    handOffIrqFdWaitersAfterRelease();',
         '    handOffIrqFdWaitersAfterRelease();\n    kernel_state_lock.unlock();'),
        ('syscalls.zig', 'asm volatile ("mfence" ::: .{ .memory = true });', 'asm volatile ("" ::: .{ .memory = true });'),
        ('syscalls.zig', '        if (user_copy.tlbShootdownActiveOnCurrentCpu()) return;\n        if (!kernel_state_lock.tryLock()) return;\n        const h = getHooks();',
         '        if (!kernel_state_lock.tryLock()) return;\n        const h = getHooks();'),
        ('syscalls.zig', '    _ = @atomicRmw(bool, &irq_fd_completion_pending, .Xchg, false, .acquire);\n', ''),
        (PATHS[0], 'lock_diagnostics.beforeAcquire(', 'bypass.beforeAcquire('),
        (PATHS[0], 'lock_diagnostics.acquired(', 'bypass.acquired('),
        (PATHS[0], 'lock_diagnostics.releasing(', 'bypass.releasing('),
        (PATHS[0], 'disableInterruptsForSchedulerLock();', 'bypassInterruptMask();'),
        (PATHS[0], 'lock_diagnostics.assertAllQuiescent();', ''),
        (PATHS[0], '.class = .thread_directory', '.class = .thread_table'),
        (PATHS[0], 'return verifiedPickThreadForCpuLocked(cpu_id);', 'return null;'),
        (PATHS[0], 'if (pinnedContextForCpu(cpu)) |ctx| return ctx.owner_process;',
         'if (threadContext(state.current_thread)) |ctx| return ctx.owner_process;'),
        (PATHS[2], 'defer diagnostic_scope.end();', ''),
        (PATHS[2], 'lock_diagnostics.assertRootEmpty();', ''),
    )
    for path, old, new in mutations:
        changed = dict(sources)
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert check(changed), (path, old)
    changed = dict(sources)
    changed[PATHS[0]] = changed[PATHS[0]].replace(
        'fn diagnosticKey(self: *SchedulerSpinLock) lock_diagnostics.order.Key {',
        'fn diagnosticKey(self: *SchedulerSpinLock) lock_diagnostics.order.Key {\n'
        'if (self == &scheduler_state.thread_table.directory_lock) return .{ .class = .thread_table };', 1)
    assert check(changed), 'earlier incorrect leaf classification hides correct branch'
    changed = dict(sources)
    changed[PATHS[1]] = changed[PATHS[1]].replace('var held = self.head;', 'var held = record.next;', 1)
    assert check(changed), 'foreign record mutable read'
    changed = dict(sources)
    changed[PATHS[0]] += '\nfn bypass(word: *u8) void { _ = @cmpxchgWeak(u8, word, 0, 1, .acquire, .monotonic); }\n'
    assert check(changed), 'late unranked CAS'
    changed = dict(sources)
    changed[PATHS[0]] = changed[PATHS[0]].replace('fn unlock(self: *SchedulerSpinLock) void {', 'fn unlock(self: *SchedulerSpinLock) void {\n _ = self.interrupts_were_enabled;', 1)
    assert check(changed), 'foreign mutable IF read'
    changed = dict(sources)
    changed[PATHS[3]] += '\nvar shared_failure: ?anyerror = null;\n'
    changed[PATHS[3]] = changed[PATHS[3]].replace('pub fn failure(err: anyerror) noreturn {', 'pub fn failure(err: anyerror) noreturn {\n shared_failure = err;', 1)
    assert check(changed), 'shared non-atomic fault write'
    count = 0
    for path in PATHS:
        for token in ('lock_diagnostics.beforeAcquire(', 'lock_diagnostics.acquired(', 'lock_diagnostics.releasing(', 'lock_diagnostics.acquiredTry(', 'lock_diagnostics.assertCanWaitForIpi();'):
            if token == 'lock_diagnostics.assertCanWaitForIpi();' and path not in ('user_copy.zig', 'smp.zig', 'scheduler_connection.zig'): continue
            if token not in sources[path]: continue
            changed = dict(sources)
            changed[path] = changed[path].replace(token, 'bypass.' + token.split('.', 1)[1], 1)
            assert check(changed), (path, token)
            count += 1
    print(f'kernel lock boundaries: scheduler/IRQ/lock ordering/lock admission and {count} acquisition/release/IPI bypass negatives PASS')


if __name__ == '__main__':
    main()
