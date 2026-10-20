#!/usr/bin/env python3
"""2a source tripwires with mutation controls; not a concurrency proof."""
from pathlib import Path
from check_kernel_address_boundaries import function_body, code_only
ROOT = Path(__file__).resolve().parents[1]

def check(files):
    errors = []
    def body(path, name):
        return function_body(code_only(files[path]), name)
    sched = 'scheduler_connection.zig'
    hint = body(sched, 'hasPendingIpcCancellations')
    pop = body(sched, 'takePendingIpcCancellation')
    push = body(sched, 'pendIpcBindingLocked')
    if '@atomicLoad(?*ipc_call_binding.Node, &ipc_cancel_head, .acquire)' not in hint:
        errors.append('pending acquire hint')
    if not pop.index('hasPendingIpcCancellations()') < pop.index('thread_table.lock()'):
        errors.append('empty drain takes T')
    if '@atomicLoad(?*ipc_call_binding.Node, &ipc_cancel_head, .acquire)' not in pop:
        errors.append('root must be reloaded under T')
    if '@atomicStore(?*ipc_call_binding.Node, &ipc_cancel_head, node.next, .release)' not in pop:
        errors.append('last pop publication')
    if '@atomicStore(?*ipc_call_binding.Node, &ipc_cancel_head, node, .release)' not in push:
        errors.append('producer publication')
    direct = body(sched, 'tryIpcDirectSwitchLocked')
    if 'source.id != state.current_thread' not in direct:
        errors.append('CPU pin scalar identity must match')
    native = 'syscall/native_ipc.zig' 
    drain = body(native, 'drainCancellations')
    if '!scheduler.hasPendingIpcCancellations() and !state.ipc_cancel_writable_pending' not in drain:
        errors.append('writable debt cannot be skipped')
    for path, name, guard, work in (
        ('syscall/capsule.zig', 'drainDeviceMemoryRetirements', 'if (state.pending_device_retirements == 0) return;', 'lockAllVmTransactions()'),
        ('syscall/ram_cache.zig', 'drain', 'if (state.pending_ram_cache_retirements == 0)', 'lockAllVmTransactions()'),
        ('memory/user_vm.zig', 'retryRetiredAddressSpaces', 'if (h.user_spaces.retired_head == null and h.user_spaces.body_storage.retired == null) return;', 'while (retired)'),
    ):
        b = body(path, name)
        if guard not in b or b.index(guard) > b.index(work): errors.append(name + ' empty guard')
    fast = body(native, 'fastDispatchAfterRegisterAdmission')
    for duplicate in ('fdEntryConst(', 'ipcMessageQueueForSend(', 'ipcMessageQueueForRecv(', 'ipcWaitListForSendPayload(', 'ipcReceiverHandoffHintForSendFd(', 'fastRegistersAdmit('):
        if duplicate in fast: errors.append('repeated authority/register work: ' + duplicate)
    for required in ('ipcUnderK(', 'borrowed.queue', 'borrowed.list', 'borrowed.hint', 'Pair.acquire('):
        if required not in fast: errors.append('authority check lost: ' + required)
    scalar = body('syscalls.zig', 'syscallIpcScalarDispatch')
    if not scalar.index('fastRegistersAdmit(frame)') < scalar.index('fastDispatchAfterRegisterAdmission('): errors.append('scalar register admission')
    return errors

if __name__ == '__main__':
    paths = ('scheduler_connection.zig', 'syscall/native_ipc.zig', 'syscall/capsule.zig', 'syscall/ram_cache.zig', 'memory/user_vm.zig', 'syscalls.zig')
    files = {p: (ROOT / 'kernel/src' / p).read_text() for p in paths}
    assert not check(files), check(files)
    mutations = (
        ('scheduler_connection.zig', 'if (source.id != state.current_thread) return false;', ''),
        ('scheduler_connection.zig', '&ipc_cancel_head, .acquire', '&ipc_cancel_head, .monotonic'),
        ('scheduler_connection.zig', '&ipc_cancel_head, node, .release', '&ipc_cancel_head, node, .monotonic'),
        ('scheduler_connection.zig', '&ipc_cancel_head, node.next, .release', '&ipc_cancel_head, node.next, .monotonic'),
        ('syscall/native_ipc.zig', 'and !state.ipc_cancel_writable_pending', ''),
        ('syscall/native_ipc.zig', 'borrowed.queue', 'unvalidated.queue'),
        ('syscall/capsule.zig', 'if (state.pending_device_retirements == 0) return;', ''),
        ('syscall/ram_cache.zig', 'if (state.pending_ram_cache_retirements == 0)', 'if (false)'),
        ('memory/user_vm.zig', 'if (h.user_spaces.retired_head == null and h.user_spaces.body_storage.retired == null) return;', ''),
    )
    for path, old, new in mutations:
        assert old in files[path]
        changed = dict(files); changed[path] = changed[path].replace(old, new)
        assert check(changed), old
    print('IPC 2a cost boundaries: PASS (9 mutation controls)')
