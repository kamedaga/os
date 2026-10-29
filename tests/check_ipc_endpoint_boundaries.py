#!/usr/bin/env python3
"""IPC writer census + scoped lock tripwires; dynamic checked races remain required."""
from pathlib import Path
import json
import re
from check_kernel_address_boundaries import code_only, without_tests, function_body
ROOT = Path(__file__).resolve().parents[1]
PATHS = ['state/ipc.zig', 'state/fd.zig', 'state/types.zig', 'scheduler_connection.zig', 'syscall/native_ipc.zig', 'sync/ipc_endpoint.zig', 'state/reference_check.zig', 'state/fd_capability.zig', 'syscalls.zig']
LOCKED = {
 'state/reference_check.zig': 'queue',
 'state/fd_capability.zig': 'deinit',
 'state/ipc.zig': 'clearIpcQueue clearIpcQueueWithFreeList clearIpcEndpointSlot clearIpcEndpointSlotWithFreeList releaseIpcChannelHandle releaseIpcChannelHandleWithFreeList clearIpcReplySlot clearIpcReplySlotWithFreeList registerIpcDirectReceive cancelIpcDirectReceivers registerIpcPollWaitersForFd registerIpcRecvCompletionWaiterForFd unregisterIpcPollWaitersForFd wakeIpcWaitersForSendFd noteIpcReceiverForFd ipcReceiverHandoffHintForSendFd takeIpcWritableWaitersForRecvFd takeIpcChannelPeerCloseWaiters ipcNotifyPeerForFd settleIpcPeerNotifyForFd consumeIpcNotifyForFd ipcNotifyWaiterForFd registerIpcNotifyWaiterForFd unregisterIpcNotifyWaiterForFd ipcChannelNotifyWaiterSnapshot takeIpcChannelNotifyWaitersAfterClose wakeIpcPollWaitersForSendFd enqueueIpcMessage ipcClaimCallReply ipcFinishCallReply releaseIpcCallAuthority ipcExtractReply ipcTakeDirectReceiver ipcCreateCall ipcCall cancelCall takeIpcCancellationWritableWaiters drainIpcCancellations ipcRecvPendingCall ipcRecv',
 'state/fd.zig': 'revokeObjectHolders fdIpcReadable fdIpcWritable fdPollEventsWithWriteMin detachFdWaitGroup retirePersistentRegistration makePersistentRegistration dmaEnablingQueue',
 'syscall/native_ipc.zig': 'fastDispatchAfterRegisterAdmission',
}
WRITES = re.compile(r'\.(?:receive_(?:head|tail|next|prev|list|target)|poll_(?:head|tail)|receiver_hints?|notify_(?:pending|waiters)|queue_key|sent|lifetime_refs)\b[^;\n]*(?:=(?!=)|\.(?:store|fetchAdd|fetchSub)\()|\.(?:appendReceive|unlinkReceive|removeWaiter|takeReadable|takeReadablePollWaiters|takeEvents|push|pop)\(')

def functions(text):
    clean = without_tests(code_only(text))
    for match in re.finditer(r'\bfn\s+(\w+)\s*\(', clean):
        name = match[1]
        # Same-name methods are intentionally recorded with their ordinal.
        tail = clean[match.start():]
        try:
            body = function_body(tail, name)
        except (AssertionError, ValueError):
            continue
        yield name, body

def census(files):
    result = {}
    for path, text in files.items():
        counts = {}
        for name, body in functions(text):
            hits = WRITES.findall(body)
            if hits: counts[name] = counts.get(name, 0) + len(hits)
        result[path] = counts
    return result

def check(files, expected):
    assert census(files) == expected, 'IPC writer census changed; audit owner, endpoint and T protocol'
    for path, names in LOCKED.items():
        for name in names.split():
            body = function_body(code_only(files[path]), name)
            assert '.acquire(' in body and '.end()' in body, (path, name, 'missing endpoint scope')
    lock = files['sync/ipc_endpoint.zig']
    assert '.class = .ipc_endpoint' in lock
    assert 'diagnostics.beforeAcquire(&self.byte.record, key)' in lock
    assert '@intFromPtr(a) < @intFromPtr(b.?)' in lock
    assert lock.index('self.second.end();') < lock.index('self.first.end();')
    ipc = files['state/ipc.zig']
    fast = function_body(code_only(files['syscall/native_ipc.zig']), 'fastDispatchAfterRegisterAdmission')
    assert fast.index('ipcUnderK(') < fast.index('Pair.acquire(') < fast.index('queue.isEmpty()')
    assert fast.index('endpoint_guard.end();', fast.index('b3.end(504') - 40) < fast.index('switchIpc(')
    cancel = function_body(code_only(ipc), 'cancelCall')
    assert cancel.count('validateIpcCancellation(') == 2
    drain = function_body(code_only(ipc), 'drainIpcCancellations')
    assert 'validateIpcCancellation(node, call.receive_target.wait_token)' in drain
    assert drain.index('finishIpcCancellation(node)') < drain.index('releaseIpcCallReference(ref)')
    pending = function_body(code_only(files['scheduler_connection.zig']), 'pendIpcBindingLocked')
    assert '.acquire(' not in pending and 'terminal_token = node.wait_token' in pending
    for name in ('bindIpcOutgoing', 'bindIpcImplicit', 'takeIpcImplicit', 'detachIpcOutgoing'):
        body = function_body(code_only(files['scheduler_connection.zig']), name)
        assert 'ipcBindingGuard(' in body and 'endpoint_guard.end()' in body
    for name in ('tryIpcFastCall', 'tryIpcFastEnqueue', 'tryIpcFastReplyRecv'):
        body = function_body(code_only(files['scheduler_connection.zig']), name)
        assert '.assertHeld();' in body and 'body.exclusion.assertHeld();' in body
    for name in ('prepareDirectNotify', 'commitDirectNotify'):
        body = function_body(code_only(files['scheduler_connection.zig']), name)
        assert 'channel.exclusion.assertHeld();' in body
    notify = function_body(code_only(files['scheduler_connection.zig']), 'prepareDirectNotify')
    assert notify.index('scheduler_state.thread_table.lock();') < notify.index('matchingThreadLocked(')
    assert 'target.active_wait_token != waiter.wait_token' in notify
    assert 'target.wait_completion_claimed' in notify
    commit = function_body(code_only(files['syscalls.zig']), 'commitDirectNotifyEntry')
    block = commit[commit.index('if (result.block_token != 0)'):]
    assert all(x in block for x in ('saveIpcLiveIfNeeded();', 'block_current_thread_for_event(', 'kernel_state_lock.unlock();', 'stagePendingSignalForUserReturn(frame);', 'return 2;'))
    assert block.index('saveIpcLiveIfNeeded();') < block.index('block_current_thread_for_event(')
    assert block.index('kernel_state_lock.unlock();') < block.index('stagePendingSignalForUserReturn(frame);') < block.index('return 2;')
    # A ready hint is not syscall provenance. Actual scheduling-oracle and
    # stale-frame negatives live in the host scheduler tests as well.
    assert '!target.entry_state.notify_sysret_continuation' in notify
    assert 'directRunnableWithSourceSelectionTime(next, &finished)' in notify
    assert 'next.linked_tree != &cpu.runqueue' in notify
    scheduler_commit = function_body(code_only(files['scheduler_connection.zig']), 'commitDirectNotify')
    park = scheduler_commit[scheduler_commit.index('if (plan.mode == .block)'):]
    sequence = ('saveIpcLiveIfNeeded();', 'captureLeavingBases(', '@memcpy(std.mem.asBytes(&source.frame)',
                'commitThreadBlockedLocked(source)', 'source.scheduler_entity.?.ownership = .blocked', 'noteCpuIdleTick(')
    assert all(item in park for item in sequence), "dedicated park missing save/publication step"
    positions = [park.index(item) for item in sequence]
    assert positions == sorted(positions), 'dedicated park save/publication order'
    assert '&result.remote_cpu' in scheduler_commit
    scheduler_source = without_tests(code_only(files['scheduler_connection.zig']))
    setters = {}
    for name, body in functions(files['scheduler_connection.zig']):
        assignments = re.findall(r'\.notify_sysret_continuation\s*=\s*([^;]+);', body)
        live = [value for value in assignments if value.strip() != 'false']
        if live: setters[name] = len(live)
    assert setters == {'commitDirectNotify': 2, 'switchToNotifyGenerationLockedWithRegisters': 1,
                       'handoffToReadyThreadGenerationInternal': 1}, 'unreviewed native-origin publisher'
    install = function_body(scheduler_source, 'installCurrentContextLocked')
    assert 'ctx.entry_state.notify_sysret_continuation = false;' in install
    assert install.index('ctx.entry_state.notify_sysret_continuation = false;') < install.index('state.current_context = ctx;')
    captures = len(re.findall(r'ctx\.frame = [^;]+;', scheduler_source))
    cleared = len(re.findall(r'ctx\.frame = [^;]+;\s*ctx\.entry_state\.notify_sysret_continuation = false;', scheduler_source))
    assert captures == cleared, 'frame replacement retained native origin'
    assert commit.index('plan.pair.end();') < commit.index('wakeCpuForNotify(')
    # No intermediate reader or retained authority may reappear. Cancellation
    # still records terminal-token debt under T and drains it only under K.
    for path in ('syscall/native_ipc.zig','scheduler_connection.zig'):
        production = without_tests(code_only(files[path]))
        assert not re.search(r'fastDispatchKFree|pinIpcFastBlock|tryIpcFastSend|releaseRetainedReply', production)
    types = files['state/types.zig']
    data_poll = function_body(code_only(types), 'dataPollAfter')
    assert 'self.assertHeld();' in data_poll
    for required in ('!node.active', 'node.binding.persistent == null', 'node.recv_memory.active',
                     'node.events & (abi.event_readable | abi.event_writable) != 0',
                     '!item.queued or item.set.invocation != null'):
        assert required in data_poll, 'data admission skipped a required observer'
    assert not WRITES.search(data_poll), 'data admission must leave close observers linked'
    # dataPollAfter also reads the K-owned FIFO/invocation. Freeze its borrower
    # surface and the K admission of the two direct entry callers.
    users = {}
    for path, text in files.items():
        for name, b in functions(text):
            count = b.count('.dataPollAfter(')
            if count: users[path + ':' + name] = count
    assert users == {
        'scheduler_connection.zig:prepareDirectHandoff': 2,
        'scheduler_connection.zig:prepareDirectNotify': 2,
        'syscall/native_ipc.zig:fastDispatchAfterRegisterAdmission': 6,
    }, 'unreviewed persistent ready FIFO borrower'
    for entry_name, prepare in (('syscallDirectIpcPrepare', 'scheduler.prepareDirectHandoff('),
                                ('prepareDirectNotifyEntry', 'scheduler.prepareDirectNotify(')):
        entry = function_body(code_only(files['syscalls.zig']), entry_name)
        assert entry.index('kernel_state_lock.tryLock()') < entry.index(prepare)
    assert 'lifetime_refs: std.atomic.Value(usize)' in types
    for name in ('appendReceive', 'unlinkReceive', 'removeWaiter', 'push', 'pop'):
        assert 'self.assertHeld();' in function_body(code_only(types), name)
    extract = function_body(code_only(files['syscall/native_ipc.zig']), 'extractReply')
    assert extract.index('takeIpcImplicit(') < extract.index('ipcExtractReply(')

if __name__ == '__main__':
    files = {p: (ROOT/'kernel/src'/p).read_text() for p in PATHS}
    manifest = ROOT/'tests/ipc_endpoint_writer_census.json'
    expected = json.loads(manifest.read_text())
    check(files, expected)
    negatives = 0
    for path, names in LOCKED.items():
        for name in names.split():
            body = function_body(files[path], name)
            broken = body.replace('.acquire(', '.injectedMissingAcquire(')
            mutated = dict(files); mutated[path] = files[path].replace(body, broken, 1)
            try: check(mutated, expected)
            except AssertionError: negatives += 1
            else: raise AssertionError(('missed lock removal', path, name))
    for path, old, new in [
        ('syscalls.zig', 'saveIpcLiveIfNeeded();', 'injectedMissingSave();'),
        ('scheduler_connection.zig', 'saveIpcLiveIfNeeded();', 'injectedMissingSave();'),
        ('scheduler_connection.zig', '!target.entry_state.notify_sysret_continuation', 'false'),
        ('scheduler_connection.zig', 'ctx.entry_state.notify_sysret_continuation = false;', 'ctx.entry_state.notify_sysret_continuation = true;'),
        ('scheduler_connection.zig', 'directRunnableWithSourceSelectionTime(next, &finished)', 'injectedWrongWinner(next, &finished)'),
        ('scheduler_connection.zig', 'next.linked_tree != &cpu.runqueue', 'false'),
        ('scheduler_connection.zig', '&result.remote_cpu', 'null'),
        ('syscalls.zig', 'stagePendingSignalForUserReturn(frame);', 'injectedMissingStage(frame);'),
        ('sync/ipc_endpoint.zig', '.class = .ipc_endpoint', '.class = .kernel_state'),
        ('sync/ipc_endpoint.zig', 'diagnostics.beforeAcquire(&self.byte.record, key)', 'injectedBeforeAcquire()'),
        ('sync/ipc_endpoint.zig', '@intFromPtr(a) < @intFromPtr(b.?)', 'false'),
        ('scheduler_connection.zig', 'body.exclusion.assertHeld();', 'injectedMissingAssertion();'),
        ('scheduler_connection.zig', 'channel.exclusion.assertHeld();', 'injectedMissingAssertion();'),
        ('scheduler_connection.zig', 'target.active_wait_token != waiter.wait_token', 'false'),
        ('state/types.zig', 'lifetime_refs: std.atomic.Value(usize)', 'lifetime_refs: usize'),
        ('scheduler_connection.zig', 'node.terminal_token = node.wait_token;', 'node.terminal_token = 0;'),
        ('state/ipc.zig', 'validateIpcCancellation(node, call.receive_target.wait_token)', 'injectedUncheckedNode(node)'),
    ]:
        mutated = dict(files); assert old in mutated[path]; mutated[path] = mutated[path].replace(old, new)
        try: check(mutated, expected)
        except AssertionError: negatives += 1
        else: raise AssertionError(('missed protocol removal', old))
    mutated = dict(files)
    mutated['syscall/native_ipc.zig'] += '\nfn accidental() void { fastDispatchKFree(); }\n'
    try: check(mutated, expected)
    except AssertionError: negatives += 1
    else: raise AssertionError('K-free root restored')
    print(f'IPC endpoint writer census PASS: {sum(len(v) for v in expected.values())} functions, {negatives} mutation controls')
