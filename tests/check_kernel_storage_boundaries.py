#!/usr/bin/env python3
"""Scoped migration/bypass checks, not a proof of lifetime or lock correctness."""
from pathlib import Path
import json
import re
from collections import Counter
from functools import lru_cache
from check_kernel_address_boundaries import code_only, function_body, without_tests

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('state/process.zig', 'scheduler_connection.zig',
         'memory/identity_allocator.zig', 'memory/free_page_list.zig',
         'state/types.zig', 'state/memory.zig', 'kernel.zig')


def check(sources):
    errors = []
    errors.extend(thread_metadata_boundary(sources['scheduler_connection.zig']))
    errors.extend(thread_body_boundary(sources['scheduler_connection.zig']))
    contracts = (
        ('state/process.zig', 'ensureProcessCapacity',
         ('descriptors.reserve(', 'descriptors.commit()', 'descriptors.rollback()', 'directory.rollback()')),
        ('scheduler_connection.zig', 'ensureThreadCapacity',
         ('ThreadGrowth.reserve(', 'publishThreadGrowthLocked(', 'transaction.rollback()', 'retryUnpublished()')),
        ('scheduler_connection.zig', 'publishThreadGrowthLocked', ('transaction.commit()',)),
    )
    for path, name, required in contracts:
        body = function_body(code_only(sources[path]), name)
        if ('allocKernelSlice(' in body or '@ptrFromInt' in body or
                any(value not in body for value in required)):
            errors.append(name + ': migrated storage transaction bypassed')
    scheduler = code_only(sources['scheduler_connection.zig'])
    draft = function_body(scheduler[scheduler.index('const ThreadGrowth ='):], 'reserve')
    if (draft.count('transaction.reserve(') != 6 or
            'scheduler_runqueue.Node, capacity - old_capacity, .stable' not in draft or
            'ThreadContext, capacity - old_capacity, .stable' not in draft or
            'ThreadSlot, capacity, .replaceable' not in draft):
        errors.append('thread draft: six extents / stable context and Node ownership missing')
    lookup = function_body(scheduler, 'threadDirectory')
    publish = function_body(scheduler, 'publishThreadGrowthLocked')
    if ('@atomicLoad(*const ThreadDirectory, &scheduler_state.thread_table.directory, .acquire)' not in lookup or
            '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release)' not in publish or
            'contexts: []const *ThreadContext' not in scheduler or
            '@memcpy(new_contexts[0..old_capacity], old.contexts)' not in publish or
            'thread_contexts_ptr' in scheduler):
        errors.append('thread directory: immutable identities / atomic root publication bypassed')
    # Count direct accesses across production, not only replacements of the
    # required atomic operations: otherwise an additional bypass would pass.
    production = without_tests(scheduler)
    if len(re.findall(r'\bscheduler_state\s*\.\s*thread_table\s*\.\s*directory\b', production)) != 2:
        errors.append('thread directory: direct root access outside acquire/release pair')
    initialize = 'draft.directory.* ='
    commit = 'transaction.commit()'
    store = '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release)'
    if (publish.count(initialize) != 1 or publish.count(commit) != 1 or publish.count(store) != 1 or
            not publish.index(initialize) < publish.index(commit) < publish.index(store) or
            re.sub(r'\s+', '', publish[publish.index(store)+len(store):]) != ';unlockThreadDirectory();queueReclaimGroupLocked(old_capacity);scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion()catch@panic();returntrue;' or
            len(re.findall(r'\blockThreadDirectory\(\);', publish)) != 1 or
            not publish.index(commit) < publish.index('lockThreadDirectory();') < publish.index(store)):
        errors.append('thread directory: initialize/commit/publish order or post-publication mutation')
    for accessor in ('threadContextLocked', 'getThreadHotState', 'getThreadHotStateConst'):
        body = function_body(scheduler, accessor)
        if body.count('threadDirectory()') != 1 or 'const directory = threadDirectory()' not in body:
            errors.append(accessor + ': lookup must use one directory snapshot')
    # Only host fixtures may export a context from this short directory loan.
    # Production borrowers hold T or a CPU pin until they finish.
    scoped = {
        'threadContextMutable': 'if(!builtin.is_test)@compileError();lockThreadDirectory();deferunlockThreadDirectory();returnthreadContextLocked(thread_index);',
        'threadContext': 'returnthreadContextMutable(thread_index);',
        'threadCapacity': 'lockThreadDirectory();deferunlockThreadDirectory();returnthreadDirectory().contexts.len;',
        'lockThreadDirectory': 'scheduler_state.thread_table.directory_lock.lock();',
        'unlockThreadDirectory': 'scheduler_state.thread_table.directory_lock.unlock();',
    }
    for accessor, expected in scoped.items():
        if re.sub(r'\s+', '', function_body(scheduler, accessor)) != expected:
            errors.append(accessor + ': short directory loan boundary bypassed')
    body = function_body(scheduler, 'ensureThreadCapacity')
    rollback = body[body.index('defer if (!transaction.finished)'):body.index('const draft =')]
    if ('thread_table.lock()' not in rollback or 'thread_table.unlock()' not in rollback):
        errors.append('thread rollback: pending mutation exclusion missing')
    adapter = code_only(sources['memory/identity_allocator.zig'])
    if '@intFromPtr' in adapter or '@ptrFromInt' in adapter:
        errors.append('storage allocator: address conversion bypass')
    acquire = function_body(adapter, 'acquire')
    release = function_body(adapter, 'release')
    if 'popContiguousOwnedTagged(pages, 4096, layout.allocation_limit, caller)' not in acquire or 'layout.identitySlice(' not in acquire:
        errors.append('storage allocator: bounded typed grant missing')
    if 'appendContiguousRange(grant.physical.toInt(), grant.pages)' not in release:
        errors.append('storage allocator: original extent/region not returned')
    raw = function_body(code_only(sources['memory/free_page_list.zig']), 'popContiguousInRange')
    if 'popContiguousOwnedInRange(' not in raw or '.physical.toInt()' not in raw or 'self.lock()' in raw:
        errors.append('PMM raw adapter must delegate to typed core')
    memory = code_only(sources['state/memory.zig'])
    kernel = code_only(sources['kernel.zig'])
    allowed = ('initPhase1InPlace', 'initFromDetectedRegionsInPlace')
    outside_constructors = memory
    for name in allowed:
        body = function_body(memory, name)
        if 'initializeStorageInPlace(self)' not in body:
            errors.append(name + ': private construction initialization missing')
        outside_constructors = outside_constructors.replace(body, '')
    if ('resetStorageInPlace' in memory + kernel or 'initializeStorageInPlace' in kernel or
            'pub fn initializeStorageInPlace' in memory or
            len(re.findall(r'\binitializeStorageInPlace\s*\(', outside_constructors)) != 1):
        errors.append('KernelState initialization: live reset / construction bypass')
    return errors


def thread_body_boundary(source):
    source = code_only(source)
    errors = []
    required = {
        'initializeThreadContextLocked': ('ctx.generation = slot.generation;', 'slot.generation >= max_thread_generation'),
        'advanceThreadSlotGenerationLocked': ('slot.generation = nextThreadGeneration(slot.generation)',),
        'ensureThreadBodyLocked': ('tx.commitAdditionalStable()', 'directory.slots[slot_index].generation'),
        'reclaimThreadBodies': ('scheduler_state.thread_table.lock()', 'group.live == 0',
            'ctx.allocated or ctx.deadline_node.linked or ctx.retry_queue != null',
            'node.ownership != .free', 'node.linked_tree != null', 'node.left != null',
            'node.right != null', 'node.parent != null', 'cpu.current_context == ctx',
            'cpu.current_entity == node', 'cpu.runqueue.root == node',
            'for (scheduler_state.cpus)', 'if (referenced) continue;',
            'retireAllocation(ThreadContext, contexts.ptr)',
            'retireAllocation(scheduler_runqueue.Node, entities.ptr)',
            'retryUnpublished()', 'reclaimRetiredAfterBorrowExclusion()'),
        'retryPendingThreadMaintenance': ('reclaimThreadBodies()',),
        'releaseThreadLocked': ('setWakeTickLocked(ctx, 0);', 'removeCompletionRetryLocked(ctx);'),
    }
    for name, tokens in required.items():
        body = function_body(source, name)
        if any(token not in body for token in tokens):
            errors.append(name + ': body lifetime / generation boundary bypassed')
    body = function_body(source, 'reclaimThreadBodies')
    order = ('lockAllCpuSchedulerStates();', 'lockAllCpuRunqueues();',
             'if (!referenced)', 'lockThreadDirectory();', '@memset(',
             'unlockThreadDirectory();', 'unlockAllCpuRunqueues();',
             'unlockAllCpuSchedulerStates();', 'retireAllocation(ThreadContext, contexts.ptr)',
             'reclaimRetiredAfterBorrowExclusion()')
    positions = [body.find(token) for token in order]
    if -1 in positions or positions != sorted(positions):
        errors.append('thread body: borrow exclusion / withdrawal / return order bypassed')
    return errors


def thread_metadata_boundary(source):
    import json
    source = production_source(source)
    expected = json.loads((ROOT / 'tests/thread-storage-caller-census.json').read_text())['readers']
    names = ('threadDirectory', 'threadContextLocked', 'getThreadHotState', 'getThreadHotStateConst')
    actual, errors = {}, []
    for match in re.finditer(r'^\s*(?:pub\s+)?(?:(?:inline|noinline)\s+)?fn\s+(\w+)\s*\(', source, re.M):
        name = match[1]
        body = function_body(source, name)
        calls = {callee: len(re.findall(r'\b' + callee + r'\(', body)) for callee in names
                 if re.search(r'\b' + callee + r'\(', body)}
        if not calls:
            continue
        kind = ('table' if 'thread_table.lock()' in body else
                'fixture' if 'if (!builtin.is_test) @compileError(' in body else
                'leaf' if name == 'threadCapacity' else 'inherited_table')
        actual[name] = dict(exclusion=kind, calls=calls)
        if kind == 'table':
            first = min(body.index(callee + '(') for callee in calls)
            if body.index('thread_table.lock()') > first:
                errors.append(name + ': metadata lookup precedes table admission')
    if actual != expected:
        errors.append('thread metadata: reader/exclusion census changed; audit required')
    for name in names:
        expected_mentions = 1 + sum(row['calls'].get(name, 0) for row in expected.values())
        if len(re.findall(r'\b' + name + r'\b', source)) != expected_mentions:
            errors.append(name + ': alias or lookup outside audited reader bodies')
    retry = function_body(source, 'snapshotThreadCapacityAndRetryReturns')
    sequence = ('thread_table.lock()', 'retryUnpublished()', 'reclaimRetiredAfterBorrowExclusion()', 'threadDirectory()')
    positions = [retry.find(item) for item in sequence]
    if (min(positions) < 0 or positions != sorted(positions) or 'lockThreadDirectory(' in retry or
            'defer scheduler_state.thread_table.unlock();' not in retry):
        errors.append('thread metadata: no-growth retry must retain table and release leaf')
    ensure = function_body(source, 'ensureThreadCapacity')
    if not re.search(r'const old_capacity = snapshotThreadCapacityAndRetryReturns\(\);\s*if \(required <= old_capacity\) return true;', ensure):
        errors.append('thread metadata: no-growth path bypasses original issuer retry')
    return errors



THREAD_METADATA_FIXTURES = ('releaseThread', 'activate', 'loadContextIntoFrame', 'wakeIfWaiting',
                'initializeThreadContext', 'threadReady', 'threadOwnedBy')
THREAD_METADATA_RETIRED = ('markThreadReady', 'setFsBase', 'setThreadGsBase', 'copySignalDeliveryConfig',
               'copyThreadXState', 'switchTo', 'switch_to_thread', 'activateOrHalt',
               'releaseStaleThreadSlot', 'tryCreateDynamicUserProcess', 'tryCreateSuspendedUserProcess',
               'generationOfThread')
THREAD_METADATA_IDENTITIES = THREAD_METADATA_FIXTURES + (
    'matchingThreadLocked', 'markThreadReadyLocked', 'releaseThreadLocked',
    'markThreadReadyGeneration', 'releaseThreadGeneration', 'setThreadBasesGeneration',
    'copyCurrentThreadStateToGeneration', 'allocateSuspendedThread', 'allocateThreadWithReadyState',
    'releaseCurrentThread', 'setCurrentFsBase', 'setCurrentGsBase', 'currentThreadGeneration',
    'setCurrentThreadBaseWithRegisters', 'setThreadBaseWithRegisters', 'threadForPrincipal',
)


def thread_identity_census(files):
    # Count identifiers, not just calls: alias and hook publication are also
    # changes to the audited authority boundary.
    result = {}
    for path, text in files.items():
        source = production_source(text)
        counts = {name: len(re.findall(r'\b' + name + r'\b', source))
                  for name in THREAD_METADATA_IDENTITIES if re.search(r'\b' + name + r'\b', source)}
        if counts:
            result[path] = counts
    return result


def thread_identity_boundary(files):
    errors = []
    scheduler = production_source(files['kernel/src/scheduler_connection.zig'])
    for path, text in files.items():
        source = production_source(text)
        for name in THREAD_METADATA_RETIRED:
            if re.search(r'\b' + name + r'\b', source):
                errors.append(path + ': retired index-only entry: ' + name)
    for name in THREAD_METADATA_FIXTURES:
        body = function_body(scheduler, name)
        if not body.strip().startswith('if (!builtin.is_test) @compileError('):
            errors.append(name + ': missing host-only admission')
    match = function_body(scheduler, 'matchingThreadLocked')
    if 'if (!ctx.allocated or ctx.generation != generation or ctx.owner_process != owner) return null;' not in match:
        errors.append('thread identity: allocated/generation/owner admission missing')
    operations = {
        'bindIpcOutgoing': ('matchingThreadLocked(', 'ctx.ipc_outgoing = node'),
        'bindIpcImplicit': ('matchingThreadLocked(', 'ctx.ipc_implicit = node'),
        'peekIpcImplicit': ('matchingThreadLocked(', 'return ctx.ipc_implicit'),
        'takeIpcImplicit': ('matchingThreadLocked(', 'ctx.ipc_implicit = null'),
        'detachIpcOutgoingLocked': ('matchingThreadLocked(', 'ctx.ipc_outgoing = null'),
        'markThreadReadyGeneration': ('matchingThreadLocked(', 'markThreadReadyLocked('),
        'releaseThreadGeneration': ('matchingThreadLocked(', 'releaseThreadLocked('),
        'setThreadBasesGeneration': ('matchingThreadLocked(', 'ctx.fs_base ='),
        'copyCurrentThreadStateToGeneration': ('matchingThreadLocked(', 'target.signal_entry ='),
    }
    for name, (admit, action) in operations.items():
        body = function_body(scheduler, name)
        tokens = ('scheduler_state.thread_table.lock();', 'defer scheduler_state.thread_table.unlock();', admit, action)
        positions = [body.find(token) for token in tokens]
        if (min(positions) < 0 or positions != sorted(positions) or
                body.count('thread_table.lock()') != 1 or body.count('thread_table.unlock()') != 1):
            errors.append(name + ': identity and action must share one table hold')
    for name in ('releaseCurrentThread', 'setCurrentThreadBaseWithRegisters', 'currentThreadGeneration',
                 'copyCurrentThreadStateToGeneration'):
        body = function_body(scheduler, name)
        if ('pinnedContextForCpu(' not in body or 'defer scheduler_state.thread_table.unlock();' not in body or
                body.find('thread_table.lock()') > body.find('pinnedContextForCpu(')):
            errors.append(name + ': current identity must use the existing pin')
    allocation = function_body(scheduler, 'allocateThreadWithReadyState')
    if ('return .{ .index = i, .generation = generation };' not in allocation or
            allocation.count('thread_table.unlock()') != 1):
        errors.append('thread allocation: generation must be returned within publication exclusion')
    for name in ('setThreadBasesGeneration', 'copyCurrentThreadStateToGeneration'):
        if 'threadSlotReferencedByCpu(' not in function_body(scheduler, name):
            errors.append(name + ': initialization must reject pinned target')
    state = production_source(files['kernel/src/state/process.zig'])
    for name in ('markThreadObjectsExitedForPrincipal', 'markThreadObjectsExitedBySlot'):
        body = function_body(state, name)
        if ('if (thread.state.isTerminal()) continue;' not in body or
                body.index('if (thread.state.isTerminal()) continue;') > body.index('thread.state = state;')):
            errors.append(name + ': old terminal object must remain immutable')
    expected = json.loads((ROOT / 'tests/thread-storage-caller-census.json').read_text())['identity_callers']
    if thread_identity_census(files) != expected:
        errors.append('thread identity: caller/alias/hook census changed; audit required')
    return errors

def cpu_pin_boundary(files):
    """Pin publication/consumption boundary; not whole-program alias analysis."""
    errors = []
    scheduler = without_tests(code_only(files['kernel/src/scheduler_connection.zig']))
    remaining = scheduler
    writers = ('installCurrentContextLocked', 'clearCurrentContextLocked',
               'initializeStaticStorage', 'buildInitialCpuSchedulerStates',
               'refreshTopology', 'setTestCurrentThread')
    for name in writers:
        body = function_body(scheduler, name)
        for field in ('current_context', 'current_generation', 'current_thread'):
            if len(re.findall(r'\.' + field + r'\s*=(?!=)', body)) != 1:
                errors.append(name + ': pin/index publication is incomplete')
        remaining = remaining.replace(body, '')
    if re.search(r'\.(?:current_context|current_generation|current_thread)\s*=(?!=)', remaining):
        errors.append('CPU pin: ownership write bypasses the audited writers')
    fixture = function_body(scheduler, 'setTestCurrentThread')
    if 'if (!builtin.is_test) @compileError(' not in fixture:
        errors.append('CPU pin: test writer can escape into production')
    for name in ('installCurrentContextLocked', 'clearCurrentContextLocked'):
        if not function_body(scheduler, name).strip().startswith('requireLocalContextWriter(state);'):
            errors.append(name + ': local IF-clear writer admission missing')
    local = function_body(scheduler, 'requireLocalContextWriter')
    for required in ('smp.currentCpuSlotOrInvalid()', 'cpu >= scheduler_state.cpus.len',
                     'state != &scheduler_state.cpus[cpu]', 'interruptsEnabled()'):
        if required not in local:
            errors.append('CPU pin: missing writer identity/IF admission: ' + required)
    accessor = function_body(scheduler, 'currentContextForXState')
    if accessor.count('pinnedContextForCpu(cpu)') != 1:
        errors.append('CPU pin: XState accessor must consume exactly one pinned identity')
    accessor += function_body(scheduler, 'pinnedContextForCpu')
    for required in ('smp.currentCpuSlotOrInvalid()', 'interruptsEnabled()',
                     'state.current_context orelse return null',
                     '!ctx.allocated or ctx.generation != state.current_generation'):
        if required not in accessor:
            errors.append('CPU pin: scalar accessor contract missing: ' + required)
    if re.search(r'\b(?:ready|threadContextMutable|threadContext|threadDirectory|currentCpuSlotIfKnown)\b', accessor):
        errors.append('CPU pin: XState accessor uses readiness or unpinned/aggregate lookup')
    smp = code_only(files['kernel/src/smp.zig'])
    strict = function_body(smp, 'currentCpuSlotOrInvalid')
    if strict.strip() != 'return x86_platform.cpuSlotForStackPointer(readStackPointer()).indexOr(cpuCapacity());':
        errors.append('CPU pin: strict scalar identity uses fallback/optional ABI')
    for name in ('threadSlotReferencedByCpu', 'threadSlotReferencedByRemoteCpu', 'controlledThreadAvailableLocked'):
        body = function_body(scheduler, name)
        if (not re.search(r'for\s*\(scheduler_state\.cpus(?:,\s*0\.\.)?\)', body) or '.current_context == ctx' not in body or
                re.search(r'\.(?:is_idle|enabled|current_generation|current_thread)\b', body)):
            errors.append(name + ': admission must respect every CPU identity pin')
    runtime = code_only(files['kernel/src/xstate_runtime.zig'])
    for name in ('saveCurrentThreadXState', 'restoreCurrentThreadXState'):
        body = function_body(runtime, name)
        if (body.count('scheduler.currentContextForXState()') != 1 or
                re.search(r'\b(?:threadContextMutable|threadContext|currentThread|threadDirectory)\s*\(', body)):
            errors.append(name + ': entry/return does not consume its CPU pin')
    for name in ('claimKernelScheduledUserEntry', 'activateGenerationLockedWithRegisters', 'installExecContextForCurrentThread'):
        if function_body(scheduler, name).count('installCurrentContextLocked(') != 1:
            errors.append(name + ': real install path lost pin transfer')
    for name in ('observeCpuIdleFromAp', 'preemptApUserThread', 'parkApThreadForBlockPreferred',
                 'noteCpuIdleTick', 'quiesceStoppedCurrentUserThreadWithRegisters', 'tryWakeWaitTransferLocked'):
        if function_body(scheduler, name).count('clearCurrentContextLocked(') != 1:
            errors.append(name + ': real idle/rollback path lost pin withdrawal')
    initializer = function_body(scheduler, 'initializeThreadContextLocked')
    initializer_wrapper = function_body(scheduler, 'initializeThreadContextWithReadyState')
    admission = 'if (threadSlotReferencedByCpu(thread_index)) return false;'
    if (admission not in initializer or
            initializer.index(admission) > initializer.index('ctx.id =') or
            'thread_table.lock()' in initializer or
            initializer_wrapper.index('thread_table.lock()') > initializer_wrapper.index('initializeThreadContextLocked(') or
            'defer scheduler_state.thread_table.unlock();' not in initializer_wrapper):
        errors.append('CPU pin: public initializer can overwrite a retiring pin')
    allocation = function_body(scheduler, 'allocateThreadWithReadyState')
    sequence = ('scheduler_state.thread_table.lock();', 'threadContextLocked(i)',
                'threadSlotReferencedByCpu(i)', 'initializeThreadContextLocked(',
                'const generation = ctx.generation;', 'verifiedAddThreadLocked(')
    positions = [allocation.find(item) for item in sequence]
    if (min(positions) < 0 or positions != sorted(positions) or
            'defer scheduler_state.thread_table.unlock();' not in allocation or
            re.search(r'\b(?:threadContext|threadContextMutable|verifiedAddThread|initializeThreadContextWithReadyState)\(', allocation)):
        errors.append('thread allocation: claim/initialize/node publication must share table exclusion')
    for name in ('initializeStaticStorage', 'buildInitialCpuSchedulerStates'):
        body = function_body(scheduler, name)
        if '.current_context = null;' not in body or '.current_generation = 0;' not in body:
            errors.append('CPU pin: bootstrap must acquire ownership at activation, not before construction')
    boot = code_only(files['kernel/src/boot/entry.zig'])
    construct = function_body(boot, 'constructBootProcesses')
    if not (construct.index('scheduler.activateNextReadyOnCurrentCpu()') <
            construct.index('@atomicStore(bool, &kernel_runtime.kernel_state_ready, true, .release);') <
            construct.index('scheduler.enableApUserDispatch();')):
        errors.append('CPU pin: boot ready/dispatch precedes first real activation')
    reset_census = {path: len(re.findall(r'\binitializeStaticStorage\b', without_tests(code_only(text))))
                   for path, text in files.items()
                   if re.search(r'\binitializeStaticStorage\b', without_tests(code_only(text)))}
    # uninterruptible wait's additional reset belongs to the compile-time host-only frontend fixture.
    expected_reset = {'kernel/src/scheduler_connection.zig': 6, 'kernel/src/boot/entry.zig': 1,
                      'kernel/src/syscalls.zig': 4, 'kernel/src/lock_diagnostics_scheduler_test.zig': 1}
    if 'comptime std.debug.assert(builtin.is_test);' not in function_body(production_source(files['kernel/src/scheduler_connection.zig']), 'testUninterruptibleWaitFrontendMatrix'):
        errors.append('CPU pin: uninterruptible wait frontend fixture must remain test-only')
    # Both IRQ host fixtures reset before/after a case; neither is callable in
    # production. Keep the exact census and require their compile-time guard.
    for fixture in ('InterruptWaiterTest', 'InterruptSnapshotTest'):
        source = files['kernel/src/syscalls.zig']
        body = function_body(source[source.index('const ' + fixture + ' = struct'):], 'run')
        if 'comptime std.debug.assert(@import("builtin").is_test);' not in body:
            errors.append('CPU pin: IRQ snapshot fixture escaped host-only admission')
    # Dynamic topology fixtures restore the borrowed bootstrap rows only in
    # host tests; the destructor itself must reject a direct production call.
    topology_fixture = scheduler[scheduler.index('const TestCpuRows = struct'):]
    if 'comptime std.debug.assert(builtin.is_test);' not in function_body(topology_fixture, 'deinit'):
        errors.append('CPU pin: dynamic topology fixture teardown escaped host-only admission')
    if reset_census != expected_reset:
        errors.append('CPU pin: static reset caller census changed (boot plus audited host fixtures only)')
    if 'scheduler.initializeStaticStorage();' not in function_body(boot, 'initializeLimineRuntimeOrHalt'):
        errors.append('CPU pin: reset moved out of bootstrap construction')
    return errors


def context_scalar_boundary(files):
    errors = []
    production = {path: without_tests(code_only(source)) for path, source in files.items()}
    scheduler = production['kernel/src/scheduler_connection.zig']
    if any(re.search(r'\bthreadXStateLoadable\b', code) for code in production.values()):
        errors.append('context scalar boundary: retired XState admission API reintroduced')
    external = {path: len(re.findall(r'\bthreadContext(?:Mutable)?\b', code))
                for path, code in production.items()
                if path != 'kernel/src/scheduler_connection.zig' and re.search(r'\bthreadContext(?:Mutable)?\b', code)}
    if external != {'kernel/src/syscalls.zig': 1}:
        errors.append('context scalar boundary: raw external borrow census changed (one host fixture remains)')
    setter = function_body(scheduler, 'setBootstrapThreadEntryForCpu')
    wrapper = function_body(scheduler, 'setBootstrapThreadEntry')
    if wrapper.strip() != 'return setBootstrapThreadEntryForCpu(smp.currentCpuSlotIfKnown(), thread_index, owner, entry, stack);':
        errors.append('context scalar boundary: public boot setter must obtain a strict known CPU')
    admission = 'if (!ctx.allocated or ctx.owner_process != owner or threadSlotReferencedByCpu(thread_index)) return false;'
    required = ('kernel_runtime.kernel_state_ready or !bootTopologyRefreshAllowed(cpu,',
                '@atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire)',
                'scheduler_state.thread_table.lock();', admission,
                'ctx.frame.rip = entry.toInt();', 'ctx.frame.rsp = stack.toInt();')
    positions = [setter.find(item) for item in required]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append('context scalar boundary: boot admission must precede both scalar writes')
    for name, helper, outputs in (
            ('currentUserEntrySnapshot', 'snapshotUserEntryForCpu', ('entry.* = .fromInt(ctx.frame.rip);', 'stack.* = .fromInt(ctx.frame.rsp);')),
            ('currentFsBaseSnapshot', 'snapshotFsBaseForCpu', ('out.* = .fromInt(ctx.fs_base);',))):
        wrapper = function_body(scheduler, name)
        if helper + '(smp.currentCpuSlotOrInvalid(), interruptsEnabled(),' not in wrapper:
            errors.append(name + ': strict current CPU / IF snapshot admission bypassed')
        body = function_body(scheduler, helper)
        if (not body.strip().startswith('if (if_enabled) return false;') or
                body.count('pinnedContextForCpu(cpu) orelse return false;') != 1 or
                any(item not in body for item in outputs)):
            errors.append(helper + ': pin admission or scalar out contract bypassed')
        if re.search(r'\b(?:lock|threadContext|threadContextMutable|threadDirectory|halt|haltWithMessage|currentContextForXState)\s*\(|@memcpy|ctx\.\*', body + wrapper):
            errors.append(helper + ': snapshot must not lock, halt, copy Context or fall back to a directory')
    boot = production['kernel/src/boot/entry.zig']
    construct = function_body(boot, 'constructBootProcesses')
    positions = [construct.find(value) for value in ('process_factory.installInitialUserStackOrHalt(',
                 'scheduler.setBootstrapThreadEntry(', 'scheduler.activateNextReadyOnCurrentCpu()')]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append('context scalar boundary: boot stack build must precede setter and activation')
    if 'scheduler.currentUserEntrySnapshot(' not in function_body(boot, 'bootWithResources'):
        errors.append('context scalar boundary: first user entry lost scalar snapshot')
    if 'scheduler.currentFsBaseSnapshot(' not in function_body(production['kernel/src/traps.zig'], 'writeCommonTrapRegs'):
        errors.append('context scalar boundary: trap diagnostic lost scalar snapshot')
    return errors


def topology_boundary(files):
    """Boot-only destructive reset tripwire; not a general call-graph proof."""
    errors = []
    sources = {path: without_tests(code_only(text)) for path, text in files.items()}
    scheduler = sources['kernel/src/scheduler_connection.zig']
    body = function_body(scheduler, 'refreshTopology')
    expected_prefix = '''if (!bootTopologyRefreshAllowed(smp.currentCpuSlotIfKnown(),
        @atomicLoad(u8, &scheduler_state.ap_user_dispatch_enabled, .acquire)))
    {
        while (true) asm volatile (           );
    }
    lockAllCpuSchedulerStates();'''
    compact = lambda text: re.sub(r'\s+', '', text)
    if not compact(body).startswith(compact(expected_prefix)):
        errors.append('topology reset: fail-closed boot admission must precede locks/mutation')
    original = function_body(files['kernel/src/scheduler_connection.zig'], 'refreshTopology')
    if 'asm volatile ("cli; hlt")' not in original:
        errors.append('topology reset: violation must stop without lock-taking diagnostics')
    # Count identifier uses as well as calls so function aliases are not a
    # silent escape from the current boot-only callsite audit.
    for name, expected in (
            ('refreshTopology', {'kernel/src/scheduler_connection.zig': 1, 'kernel/src/boot/entry.zig': 1}),
            ('enableApUserDispatch', {'kernel/src/scheduler_connection.zig': 1, 'kernel/src/boot/entry.zig': 1}),
            ('ap_user_dispatch_enabled', {'kernel/src/scheduler_connection.zig': 5})):
        actual = {path: len(re.findall(r'\b' + name + r'\b', code)) for path, code in sources.items()
                  if re.search(r'\b' + name + r'\b', code)}
        if actual != expected:
            errors.append(name + ': boot publication/reset census changed')
    enable = function_body(scheduler, 'enableApUserDispatch')
    if compact(enable) != compact('@atomicStore(u8, &scheduler_state.ap_user_dispatch_enabled, 1, .release);'):
        errors.append('topology reset: dispatch publication must remain monotonic')
    boot = sources['kernel/src/boot/entry.zig']
    if ('scheduler.refreshTopology();' not in function_body(boot, 'initializeLimineRuntimeOrHalt') or
            'scheduler.enableApUserDispatch();' not in function_body(boot, 'constructBootProcesses') or
            'constructBootProcesses(state, resources);' not in function_body(boot, 'bootWithResources')):
        errors.append('topology reset: boot phase ownership changed')
    entry = sources['bootloader/limine/kernel_entry.zig']
    first = entry.find('entry.initializeLimineRuntimeOrHalt(smp_resources);')
    second = entry.find('entry.bootWithResources(resources, maybeResetAtCheckpoint);')
    if first < 0 or second < 0 or first >= second:
        errors.append('topology reset: runtime setup must precede process/user dispatch setup')
    return errors


def pmm_boundary(files):
    errors = []
    leaf_path = 'kernel/src/memory/free_page_list.zig'
    leaf = files[leaf_path]
    production = without_tests(code_only(leaf))
    imports = []
    masked = code_only(leaf)
    for match in re.finditer(r'@import\("([^"\n]+)"\)', leaf):
        if masked[match.start():].startswith('@import('):
            imports.append(match[1])
    if sorted(imports) != sorted(('std', 'builtin', 'page_cache.zig', 'reclaim_cache.zig', 'ordered_index.zig', '../smp.zig', 'owned_storage.zig', '../state/errors.zig', '../sync/lock_diagnostics.zig', '../lock_profile.zig', '../arch/x86_64/boot_hhdm.zig', '../arch/x86_64/physical_layout.zig', 'physical_access_test.zig', 'physical_access_test.zig', 'physical_access_test.zig', 'physical_access_test.zig', 'physical_access_test.zig')):
        errors.append('PMM leaf has an owner/driver dependency or lost its shared error vocabulary')
    cache_source = without_tests(files['kernel/src/memory/reclaim_cache.zig'])
    imports = re.findall(r'@import\("([^"\n]+)"\)', cache_source)
    if sorted(imports) != sorted(('std', 'owned_storage.zig', 'page_cache.zig')):
        errors.append('reclaim cache: owner/scheduler dependency')
    registry = function_body(code_only(cache_source), 'reclaim')
    if 'domain.reclaim()' not in registry or any(x in registry for x in ('kernel_state_lock', 'reclaimFdCapabilities', '.boundary(', '.retire(')):
        errors.append('OOM may reclaim only detached physical grants')
    queue = code_only(files['kernel/src/memory/owned_storage.zig']).split('pub const RetiredQueue = struct {')[1].split('pub fn requiredPages')[0]
    reclaim_queue = function_body(queue, 'reclaim')
    if ('allocator.returnRecord(record, extent)' not in reclaim_queue or
            reclaim_queue.index('self.lock.unlock();') > reclaim_queue.index('allocator.returnRecord')):
        errors.append('detached grant return or unlocked issuer return omitted')
    cache_body = cache_source[cache_source.index('pub const Cache = struct'):]
    reclaim = function_body(code_only(cache_body), 'reclaim')
    if ('self.head = null;' not in reclaim or 'self.lock.unlock();' not in reclaim or
            reclaim.index('self.head = null;') > reclaim.index('self.lock.unlock();') or
            reclaim.index('self.lock.unlock();') > reclaim.index('backing.release(')):
        errors.append('reclaim cache: grants must detach and leaf must end before issuer return')
    page = function_body(production, 'appendPage')
    if 'return self.appendContiguousRange(pa, 1);' not in page or 'quarantined(' in page:
        errors.append('appendPage: must delegate once to checked range return')
    for name, query in (('appendContiguousRange', 'quarantined(start, pages)'),):
        body = function_body(production, name)
        if body.count('quarantined(') != 1 or query not in body or body.index(query) > body.index('self.lock()'):
            errors.append(name+': quarantine query count/arguments/pre-lock boundary changed')
    types = files['kernel/src/state/types.zig']
    # Imports are checked with their original strings, but comments cannot
    # satisfy a contract. This pins one composition point, not dynamic hooks.
    selected = 'pub const FreePageList = @import("../memory/free_page_list.zig").WithQuarantine(@import("../iommu.zig").physicalRangeQuarantined);'
    if selected not in types or not code_only(types)[types.index(selected):].startswith('pub const FreePageList'):
        errors.append('production PMM quarantine selection bypassed')
    if ('pub const FreePageList = struct' in code_only(types) or
            'pub const RegionFreeRange = struct' in code_only(types)):
        errors.append('PMM source of truth duplicated in state types')
    references = {}
    for path, source in files.items():
        source = without_tests(code_only(source))
        count = len(re.findall(r'\bWithQuarantine\b', source)) - len(re.findall(r'\bfn\s+WithQuarantine\b', source))
        if count:
            references[path] = count
    if 'const TestList = if (builtin.is_test) WithQuarantine(' not in leaf or re.search(r'\bTestList\b', production[:production.index('const TestList')]):
        errors.append('host PMM fixture referenced by production implementation')
    if references != {'kernel/src/state/types.zig': 1, leaf_path: 1}:
        errors.append('PMM concrete type reference census changed; audit quarantine selection: '+str(references))
    for path, raw in files.items():
        source = without_tests(code_only(raw))
        for match in re.finditer(r'\.(appendPage|appendContiguousRange)\s*\(', source):
            depth, commas, at = 1, 0, match.end()
            while at < len(source) and depth:
                ch = source[at]
                if ch in '([{': depth += 1
                elif ch in ')]}': depth -= 1
                elif ch == ',' and depth == 1: commas += 1
                at += 1
            args = source[match.end():at-1].strip().rstrip(',').strip()
            # Count top-level separators again without an optional trailing comma.
            depth, count = 0, 1 if args else 0
            for ch in args:
                if ch in '([{': depth += 1
                elif ch in ')]}': depth -= 1
                elif ch == ',' and depth == 0: count += 1
            if count != (1 if match[1] == 'appendPage' else 2):
                errors.append(path + ': caller-supplied PMM region argument')
        if path not in (leaf_path, 'kernel/src/memory/pmm.zig', 'bootloader/limine/resources.zig') and re.search(r'\.installRegions\s*\(', source):
            errors.append(path + ': runtime PMM catalog installation')
        if path not in (leaf_path, 'kernel/src/memory/pmm.zig') and re.search(r'\.seedContiguousRange\s*\(', source):
            errors.append(path + ': runtime initial-page seeding')
    if re.search(r'\b(?:appendRegion|canAppendPage|appendGrantedRange|RegionMismatch|RegionBoundaryCrossed)\b', production):
        errors.append('obsolete/caller-region PMM API restored')
    body = function_body(production, 'appendLocked')
    overlap = body.find('return error.FreeRangeOverlap')
    if overlap < 0 or re.search(r'region_id[^;{}]*continue', body[:overlap]):
        errors.append('PMM overlap must precede region-restricted merging')
    if 'if (endOf(descriptor(pa).?.*) > start) return error.FreeRangeOverlap;' not in body or 'if (pa < end) return error.FreeRangeOverlap;' not in body:
        errors.append('PMM predecessor/successor overlap check missing')
    admission = function_body(production, 'appendContiguousRange')
    if 'try self.ownership.reserve(span);' not in admission or 'errdefer self.ownership.move(span, .reserved, .allocated);' not in admission:
        errors.append('PMM cache return ownership reservation/rollback missing')
    for name in ('drainOneLocked', 'drainLocked', 'drainMatchingLocked'):
        if 'quarantined(' in function_body(production, name):
            errors.append('PMM cache drain invokes driver under cache/P')
    cache = without_tests(code_only(files['kernel/src/memory/page_cache.zig']))
    reserve = function_body(cache, 'reserve')
    if 'count = claimed' not in reserve or '.reserved, .allocated' not in reserve or '!self.change(span_.first + claimed, .allocated, .reserved)' not in reserve:
        errors.append('PMM reservation must reject duplicates and roll back only its claimed prefix')
    return errors


def vmo_storage_boundary(files):
    """Owner-local publication, callback exclusion and rejected-return ownership."""
    errors = []
    source = without_tests(code_only(files['kernel/src/memory/vmo_backing.zig']))
    compact = lambda text: re.sub(r'\s+', '', text)
    ensure = compact(function_body(source, 'ensureLocked'))
    release = compact(function_body(source, 'release'))
    retry = compact(function_body(source, 'reclaim'))
    clear = compact(function_body(source, 'clear'))
    required = ('self.lock.unlock();vardraft=reservePath(missing,allocator)',
                'self.lock.lock();constvalid=self.matches(ref,ref.owner);',
                'if(validandnow!=nullandnow.?>=missing)self.publishPathLocked(index,&draft);',
                'self.lock.unlock();discardPath(draft);self.lock.lock();')
    reserve = compact(function_body(source, 'reservePath'))
    fault = compact(function_body(source, 'resolveOrInsert'))
    if (any(token not in ensure for token in required) or
        'errdeferdiscardPath(draft);' not in reserve or
        'tryacquire(allocator,depth==levels+1)' not in reserve):
        errors.append('VMO path preparation/publication must revalidate under owner lock with callbacks outside')
    fault_required = ('self.lock.unlock();if(has_parent){constparent=tryinherited(context);',
                      'self.lock.lock();constvalid=self.matches(ref,ref.owner);',
                      'now.?>=missing.?)self.publishPathLocked(index,&draft);',
                      'self.lock.unlock();discardPath(draft);')
    if any(token not in fault for token in fault_required):
        errors.append('VMO fault resolution must keep parent/allocation/return outside owner lock and revalidate publication')
    if ('constallocator=p.allocator;' not in release or 'constgrant=p.grant;' not in release or
        'allocator.release(allocator.context,grant)' not in release or
        'pending_lock.lock();p.next=pending;pending=p;' not in release):
        errors.append('VMO return must preserve original issuer/grant and retain failed pages')
    if 'while(list)|p|{list=p.next;release(p);}' not in retry:
        errors.append('VMO finite pending retry must save next before return')
    if 'self.root=null;' not in clear or 'self.lock.unlock();if(root)|node|self.destroyNode(node,0);' not in clear:
        errors.append('VMO root must detach before unlocked metadata return')
    for path, original in files.items():
        if re.search(r'\bvmo_backing_page_store\b', without_tests(code_only(original))):
            errors.append(path + ': obsolete global backing array borrow')
    return errors


@lru_cache(maxsize=512)
def production_source(source):
    return without_tests(code_only(source))


def as_directory_boundary(files):
    errors = []
    path = 'kernel/src/memory/address_space.zig'
    source = production_source(files[path])
    get = function_body(source, 'get')
    grow = function_body(source, 'ensureCapacity')
    slot = function_body(source, 'ensureSlot')
    def ordered(body, parts):
        cursor = 0
        for part in parts:
            found = body.find(part, cursor)
            if found < 0:
                return False
            cursor = found + len(part)
        return True
    borrow = function_body(source, 'borrow')
    if not ordered(borrow, ('self.lockDirectory()', 'self.getLocked(index)', 'if (space.retiring', 'space.borrowers += 1')):
        errors.append('AS loan: publication check and pin must share AD')
    reclaim = function_body(source, 'reclaimRetiredBody')
    if not ordered(reclaim, ('space.borrowers == 0', 'self.extra_slots[', 'self.unlockDirectory(', 'self.body_storage.retireAllocation(')):
        errors.append('AS body: loans must end before withdrawal and return outside AD')
    vm = production_source(files['kernel/src/memory/user_vm.zig'])
    if not ordered(function_body(vm, 'lockAddressSpace'), ('user_spaces.borrow(', 'lockState(', 'if (space.retiring)')):
        errors.append('AS lock: lookup-to-A loan or retirement revalidation missing')
    if not ordered(function_body(vm, 'unlockAddressSpace'), ('unlockState(', 'user_spaces.endBorrow(')):
        errors.append('AS lock: loan ended before A release')
    reaper = function_body(vm, 'retryRetiredAddressSpaces')
    if not ordered(reaper, ('scheduler.addressSpaceUsersEnded(', 'user_spaces.loansEnded(', '.pt_store.freeDetached()', 'reservationStorageReturned(', 'user_spaces.reclaimRetiredBody(')):
        errors.append('AS retire: context/CPU/loan/child exclusion before return missing')
    if not ordered(get, ('self.lockDirectory()', 'defer self.unlockDirectory(', 'self.extra_slots[')):
        errors.append('AS directory lookup escapes exclusion')
    if not ordered(grow, ('&self.directory_storage', 'transaction.reserve(', 'self.lockDirectory()',
                          '@memcpy(', 'transaction.commit()', 'self.extra_slots = new_extra.ptr',
                          'self.unlockDirectory(', 'self.retryDetachedDirectoryReturns()')):
        errors.append('AS directory publication/reclaim ordering bypassed')
    if not ordered(grow, ('self.retryDetachedDirectoryReturns()', 'if (required <= old_capacity)')):
        errors.append('AS directory no-growth retry missing')
    retry = function_body(source, 'retryDetachedDirectoryReturns')
    if 'self.directory_storage.reclaimRetiredAfterBorrowExclusion()' not in retry or 'lockDirectory(' in retry:
        errors.append('AS directory retry must run outside leaf')
    if not ordered(slot, ('&self.body_storage', 'transaction.reserve(UserAddressSpace, 1, .stable)',
                          'self.lockDirectory()', 'const entry = &self.extra_slots[',
                          'if (entry.* != 0)', 'transaction.commitAdditionalStable()', 'entry.* = @intFromPtr(space)',
                          'self.unlockDirectory(')):
        errors.append('AS body stable publication bypassed')
    if not ordered(slot, ('if (types.processPrincipalFromIndex(index) == null) return null;', 'index + 1')):
        errors.append('AS slot index overflow admission missing')
    lock = function_body(files[path], 'lockDirectory')
    unlock = function_body(files[path], 'unlockDirectory')
    if not ordered(lock, ('if (!builtin.is_test)', '"pushfq; pop %[flags]; cli"', '@cmpxchgWeak')):
        errors.append('AS directory IF-save/CLI before acquisition missing')
    if not ordered(unlock, ('@atomicStore(u8, &self.directory_lock, 0, .release)',
                            'if (!builtin.is_test and restore_interrupts)', 'asm volatile ("sti"')):
        errors.append('AS directory release-before-IF-restore missing')
    if unlock.find('asm volatile ("sti"') < unlock.find('@atomicStore('):
        errors.append('AS directory enabled interrupts before release')
    for body in (grow, slot[slot.index('self.body_storage.retryUnpublished'):]):
        hold = body[body.find('self.lockDirectory()'):body.rfind('self.unlockDirectory(')]
        if any(word in hold for word in ('self.get(', 'self.capacity(', 'transaction.reserve(',
                                         'retryUnpublished(', 'reclaimRetired', 'lockAddressSpace(',
                                         'retryDetachedDirectoryReturns(', '@panic(', 'free_list.', 'callback')):
            errors.append('AS directory leaf lock gained external work')
    if len(re.findall(r'\.\s*extra_slots\b', source)) != 7:
        errors.append('AS directory added module-local raw reader/writer')
    for name, count in (('initializeFreshStorage', 1), ('get', 1), ('ensureCapacity', 2), ('ensureSlot', 1), ('getLocked', 1), ('reclaimRetiredBody', 1)):
        if function_body(source, name).count('self.extra_slots') != count:
            errors.append('AS directory direct-root census changed: ' + name)
    # FD entry grants must retain rejected returns after K borrow exclusion.
    slot = code_only(files['kernel/src/memory/slot_table.zig'])
    if 'owner.transferRetiredAfterBorrowExclusion(domain)' not in function_body(slot, 'reclaim'):
        errors.append('canonical FD entry reclaim bypasses detached ownership')
    if '@atomicStore(?*Node, &self.root, null, .release);' not in function_body(slot, 'retire'):
        errors.append('entry retirement omitted directory withdrawal')
    for other, original in files.items():
        production = production_source(original)
        if other != path and re.search(r'\.\s*extra_slots\b', production):
            errors.append(other + ': AS directory escaped owner module')
        count = len(re.findall(r'\breclaimRetiredAfterBorrowExclusion\b', production))
        expected = 2 if other == 'kernel/src/state/process.zig' else 3 if other == 'kernel/src/scheduler_connection.zig' else 2 if other == path else 1 if other in ('kernel/src/memory/user_vm.zig', 'kernel/src/memory/owned_storage.zig', 'kernel/src/memory/slot_table.zig', 'kernel/src/memory/dma_retention.zig') else 0
        if count != expected:
            errors.append(other + ': unaudited retired-storage reclamation caller/alias')
        count = len(re.findall(r'\bcommitAdditionalStable\b', production))
        expected = 5 if other == 'kernel/src/memory/slot_table.zig' else 2 if other == 'kernel/src/state/ipc.zig' else 1 if other == 'kernel/src/state/process.zig' else 1 if other in (path, 'kernel/src/memory/owned_storage.zig', 'kernel/src/scheduler_connection.zig', 'kernel/src/memory/dma_retention.zig') else 0
        if count != expected:
            errors.append(other + ': unaudited stable-append caller/alias')
        constructors = {
            path: 1, 'kernel/src/boot/entry.zig': 1,
            'kernel/src/mmio_overlay_test.zig': 1,
        }
        if len(re.findall(r'\binitializeFreshStorage\b', production)) != constructors.get(other, 0):
            errors.append(other + ': AS fresh-construction caller/alias census changed')
    return errors


def vma_directory_boundary(files):
    errors = []
    path = 'kernel/src/state/process.zig'
    source = production_source(files[path])
    grow = function_body(source, 'ensureProcessCapacity')
    lookup = function_body(source, 'vmaTableForProcessIndex')
    const_lookup = function_body(source, 'vmaTableForProcessIndexConst')
    if re.sub(r'\s+', '', const_lookup).strip('{}') != 'returnvmaTableForProcessIndex(@constCast(self),index);':
        errors.append('VMA const lookup must delegate to the excluded dynamic lookup')
    def ordered(body, parts):
        cursor = 0
        for part in parts:
            found = body.find(part, cursor)
            if found < 0:
                return False
            cursor = found + len(part)
        return True
    if not ordered(lookup, ('if (index < process_count)', 'lockVmaDirectory(self)',
                            'defer unlockVmaDirectory(', 'self.vma_tables_extra.len',
                            'return self.vma_tables_extra[extra]')) or 'process_capacity' in lookup:
        errors.append('VMA lookup: root and bounds must share leaf exclusion')
    if not ordered(grow, ('retryVmaDirectoryReturns(self)', 'if (required <= self.process_capacity)',
                          'directory.reserve(*VmaTable, extra_count, .replaceable)',
                          'bodies.reserve(VmaTable, extra_count - old_extra_count, .stable)',
                          'lockVmaDirectory(self)', '@memcpy(new_vma[0..old_extra_count], self.vma_tables_extra)',
                          'bodies.commitAdditionalStable()', 'directory.commit()',
                          'self.vma_tables_extra = new_vma', 'unlockVmaDirectory(self, interrupts)',
                          'retryVmaDirectoryReturns(self)')):
        errors.append('VMA growth: reserve/stable ownership/publication/reclaim ordering')
    held = grow[grow.find('const interrupts = lockVmaDirectory'):grow.find('unlockVmaDirectory(self, interrupts)')]
    if any(word in held for word in ('return false', '.reserve(', '.rollback(', '@panic(', 'reclaimRetired', 'free_list.')):
        errors.append('VMA growth: fallible allocation/external work in publication leaf')
    retry = function_body(source, 'retryVmaDirectoryReturns')
    if ('self.vma_directory_storage.reclaimRetiredAfterBorrowExclusion()' not in retry or
            'lockVmaDirectory(' in retry or 'self.process_storage' in retry):
        errors.append('VMA reclaim: directory-only owner, outside reader leaf')
    lock = function_body(files[path], 'lockVmaDirectory')
    unlock = function_body(files[path], 'unlockVmaDirectory')
    if not ordered(lock, ('"pushfq; pop %[flags]; cli"', '@cmpxchgWeak')):
        errors.append('VMA leaf: IF-save/CLI missing')
    if not ordered(unlock, ('@atomicStore(u8, &self.vma_directory_lock.value, 0, .release)',
                            'if (!builtin.is_test and interrupts)', 'asm volatile ("sti"')):
        errors.append('VMA leaf: release before interrupt restore required')
    if len(re.findall(r'\.\s*vma_tables_extra\b', source)) != 4:
        errors.append('VMA directory raw-access census changed')
    for other, original in files.items():
        if other.startswith('kernel/src/') and other not in (path, 'kernel/src/state/memory.zig'):
            if re.search(r'\.\s*vma_tables_extra\b', production_source(original)):
                errors.append(other + ': VMA directory reader bypass')
    return errors


def descriptor_boundary(files):
    errors = []
    path = 'kernel/src/state/process.zig'
    source = production_source(files[path])
    if 'pub fn descriptorSlotLocked' in source or 'process_state.processDescriptorSlot;' in files['kernel/src/kernel.zig']:
        errors.append('descriptor mutable loan escaped owner module')
    if '?*const ProcessDescriptor' in source or 'pub fn processDescriptorSlot(' in source:
        errors.append('descriptor external raw borrow reintroduced')
    slot = function_body(source, 'descriptorSlotLocked')
    if 'process_capacity' in slot or 'self.process_descriptors_extra.len' not in slot:
        errors.append('descriptor bounds must belong to protected root')
    capacity = function_body(source, 'processCapacitySnapshot')
    compact_capacity = re.sub(r'\s+', '', capacity).strip('{}')
    if compact_capacity != ('constmutable=@constCast(self);constinterrupts=lockDescriptors(mutable);'
                            'deferunlockDescriptors(mutable,interrupts);returnself.process_capacity;'):
        errors.append('descriptor capacity snapshot bypassed exclusion')
    if len(re.findall(r'\.\s*process_descriptors\b', source)) != 1 or slot.count('self.process_descriptors[') != 1:
        errors.append('descriptor inline root escaped locked accessor')
    for name in ('isActiveProcess', 'processStatus', 'isBootstrapOwner', 'processSignalPendingHintVa', 'setProcessSignalPendingHintVa',
                 'processDescriptorSlotConst', 'createProcessDescriptorBelowChecked',
                 'ensureProcessDescriptor', 'setBootstrapOwner', 'markProcessFaulted',
                 'markProcessExited', 'removeProcessDescriptor', 'clearPrincipalState', 'releaseProcessTimer', 'recordPrincipalSyscall'):
        body = function_body(source, name)
        if not re.search(r'\blockDescriptors\(', body) or not re.search(r'\bunlockDescriptors\(', body):
            errors.append(name + ': descriptor leaf bypass')
        if body.find('lockDescriptors(') > body.find('descriptorSlotLocked('):
            errors.append(name + ': raw slot obtained before exclusion')
        if 'debug_process_lifecycle_hook' in body and body.rfind('unlockDescriptors(') > body.find('debug_process_lifecycle_hook'):
            errors.append(name + ': debug hook under descriptor leaf')
    allowed = {
        'isActiveProcess': 1, 'processStatus': 1, 'processSignalPendingHintVa': 1,
        'setProcessSignalPendingHintVa': 1, 'isBootstrapOwner': 1,
        'processDescriptorSlotConst': 1, 'tryProcessLabel': 1,
        'createProcessDescriptorBelowChecked': 1, 'ensureProcessDescriptor': 2,
        'setBootstrapOwner': 1, 'markProcessFaulted': 1, 'markProcessExited': 1,
        'removeProcessDescriptor': 1, 'clearPrincipalState': 1, 'releaseProcessTimer': 1,
        'recordPrincipalSyscall': 1,
    }
    if len(re.findall(r'\bdescriptorSlotLocked\b', source)) != 1 + sum(allowed.values()):
        errors.append('descriptor private loan caller census changed')
    for name, count in allowed.items():
        if function_body(source, name).count('descriptorSlotLocked(') != count:
            errors.append(name + ': audited descriptor borrow count changed')
    label = function_body(source, 'tryProcessLabel')
    if ('@cmpxchgStrong' not in label or 'lockDescriptors(' in label.replace('unlockDescriptors(', '') or
            'return null;' not in label or 'defer unlockDescriptors(' not in label):
        errors.append('diagnostic descriptor lookup must not block on exception reentry')
    if '.tryProcessLabel(principal)' not in files['kernel/src/kernel_runtime.zig']:
        errors.append('trap diagnostic must use nonblocking label snapshot')
    disable = function_body(files[path], 'disableDescriptorInterrupts')
    lock = function_body(source, 'lockDescriptors')
    unlock = function_body(source, 'unlockDescriptors')
    if '"pushfq; pop %[flags]; cli"' not in disable:
        errors.append('descriptor IF-save/CLI missing')
    if lock.find('disableDescriptorInterrupts()') < 0 or lock.find('disableDescriptorInterrupts()') > lock.find('@cmpxchgWeak'):
        errors.append('descriptor IRQ disable must precede acquisition')
    if unlock.find('@atomicStore(u8, &self.descriptor_lock.value, 0, .release)') < 0 or unlock.find('@atomicStore') > unlock.find('restoreDescriptorInterrupts(interrupts)'):
        errors.append('descriptor release must precede IF restore')
    grow = function_body(source, 'ensureProcessCapacity')
    for first, last in (('unlockVmaDirectory(self, interrupts)', 'lockDescriptors(self)'),
                        ('descriptors.commit()', 'self.process_descriptors_extra = new_desc'),
                        ('self.process_descriptors_extra = new_desc', 'self.process_capacity = capacity'),
                        ('self.process_capacity = capacity', 'unlockDescriptors(self, descriptor_interrupts)')):
        if first not in grow or last not in grow or grow.index(first) >= grow.index(last):
            errors.append('descriptor publication order: ' + first)
    retry = function_body(source, 'retryDescriptorReturns')
    if 'self.descriptor_storage.reclaimRetiredAfterBorrowExclusion()' not in retry or 'lockDescriptors(' in retry:
        errors.append('descriptor-only reclamation outside leaf required')
    for name, original in files.items():
        if not name.startswith('kernel/src/'):
            continue
        body = production_source(original)
        if name not in (path, 'kernel/src/state/memory.zig') and re.search(r'\.\s*process_descriptors(?:_extra)?\b', body):
            errors.append(name + ': raw descriptor storage bypass')
    if len(re.findall(r'\.\s*process_descriptors_extra\b', source)) != 4:
        errors.append('descriptor root census changed')
    return errors


def fd_storage_boundary(files):
    """Storage invariants; caller/lock exclusion still needs separate audit."""
    errors = []
    # Audited direct accessors/lock-drop surface, including aliases. This
    # tripwire requires review when callers change; counts are NOT a borrow
    # proof and do not replace auditing last use in each existing caller.
    pattern = r'\b(?:getFdTable(?:Const)?|fdTableForProcessIndex(?:Const)?|fdTableForActiveProcess(?:Const)?|fdEntryConst|fdPayloadWithRights(?:Const)?|ensureFdTableCapacity|releaseFdStorage|reclaimFdStorage|before_current_thread_leave|reacquire_kernel_state_lock)\b'
    expected = json.loads((ROOT / 'tests/fd-storage-caller-census.json').read_text())
    actual = {}
    for name, original in files.items():
        if name.startswith('kernel/src/'):
            counts = Counter(re.findall(pattern, production_source(original)))
            if counts:
                actual[name] = dict(counts)
    if actual != expected:
        errors.append('FD borrower/lock-drop caller census changed; audit before accepting')
    path = 'kernel/src/state/process.zig'
    source = production_source(files[path])
    grow = function_body(source, 'ensureProcessCapacity')
    required = ('while (self.fd_tables_extra.live < extra_count)',
                'self.fd_tables_extra.allocate(identity_allocator.allocator(free_list), &self.object_directory_leaf',
                '@atomicStore(usize, &self.fd_process_capacity, capacity, .seq_cst)', 'retryFdDirectoryReturns(self)')
    if any(value not in grow for value in required) or 'new_fd' in grow:
        errors.append('FD directory must append stable canonical tables')
    retry = function_body(source, 'retryFdDirectoryReturns')
    if 'self.fd_tables_extra.reclaim()' not in retry:
        errors.append('FD directory return retry missing')
    entries = function_body(source, 'ensureFdTableCapacity')
    if 'table.ensureCapacity(' not in entries or 'identity_allocator.allocator(free_list)' not in entries:
        errors.append('FD entry growth must use the owned SlotTable allocator')
    types = production_source(files['kernel/src/state/types.zig'])
    table = types[types.index('pub const FdTable = struct {'):types.index('pub const FdTransferMode')]
    grow_entries = function_body(table, 'ensureCapacity')
    required = ('slot_storage.SlotTable(FdEntry)', 'self.entries.allocate(allocator, leaf,',
                'while (self.entries.live < target)', '@atomicStore(usize, &self.capacity, target, .release)')
    if (any(x not in table for x in required) or '@memcpy' in table or
            grow_entries.index('@atomicStore(usize, &self.capacity, target, .release)') < grow_entries.index('self.entries.allocate(allocator, leaf,')):
        errors.append('FD entries must be stable and capacity published after preparation')
    if 'table.entries.reclaim()' not in function_body(source, 'reclaimFdStorage'):
        errors.append('FD directory returns must retain original grants')
    retire = function_body(source, 'releaseFdStorage')
    if not all(value in retire for value in ('self.retireFdStorage(table)', 'reclaimFdStorage(table, free_list)')):
        errors.append('FD teardown must retire publication without wiping pending grants')
    close = re.sub(r'\s+', '', function_body(production_source(files['kernel/src/state/vma.zig']), 'releaseFdTableForProcessIndexWithFreeList'))
    if (close.count('releaseFdStorage') != 1 or not close.endswith(
            'self.releaseKernelObjectWithFreeList(object_ref,free_list);}'
            'self.releaseFdStorage(table,free_list);')):
        errors.append('FD entry storage must outlive the entire capability-close loop')
    for other, original in files.items():
        if not other.startswith('kernel/src/'):
            continue
        body = production_source(original)
        if other not in (path, 'kernel/src/state/memory.zig', 'kernel/src/state/fd_capability.zig', 'kernel/src/kernel.zig') and re.search(r'\.\s*fd_tables(?:_extra)?\b', body):
            errors.append(other + ': FD raw directory bypass')
    policy = function_body(production_source(files['kernel/src/syscall/lock_policy.zig']), 'needsKernelStateLock')
    expected_policy = '''if (nr == runtime_abi.syscall_getpid or nr == runtime_abi.syscall_gettid or
        nr == runtime_abi.syscall_clock_getres or nr == runtime_abi.syscall_yield or
        nr == runtime_abi.syscall_nanosleep or nr == runtime_abi.syscall_futex_wait or
        nr == runtime_abi.syscall_futex_wake or nr == runtime_abi.syscall_futex_wake_wait or
        nr == runtime_abi.syscall_futex_requeue) return false;
        if (nr == fd_abi.syscall_vmo_revoke) return true;
        return process_abi.isProcessSyscall(nr) or
        (nr >= pager_abi.syscall_pager_port_create and nr <= pager_abi.syscall_pager_fail) or
        capsule_abi.isCapsuleSyscall(nr) or
        fd_abi.isFdSyscall(nr) or ipc_abi.isIpcSyscall(nr) or runtime_abi.isRuntimeSyscall(nr) or
        (nr >= vm_abi.syscall_vm_first and nr <= vm_abi.syscall_vm_last);'''
    if re.sub(r'\s+', '', policy).strip('{}') != re.sub(r'\s+', '', expected_policy):
        errors.append('FD borrowing requires reviewed KernelState admission policy')
    # A successful block drops KernelState. Only the saved return frame may
    # be used afterwards; failure branches still own the lock and may cancel.
    for file, fn in (('syscall/fd.zig', 'fdWaitMany'),
                     ('syscall/native_ipc.zig', 'recvWaitArgs'),
                     ('syscall/native_ipc.zig', 'callSync'),
                     ('syscall/native_ipc.zig', 'notifyWait')):
        body = re.sub(r'\s+', '', function_body(production_source(files['kernel/src/' + file]), fn))
        terminal = (r'switchIpc\([^;]+?\)\)(?:\{returnframe\.rax;\}|returnframe\.rax;)'
                    if fn in ('recvWaitArgs', 'callSync') else
                    r'h\.before_current_thread_leave,?\)\)(?:\{returnframe\.rax;\}|returnframe\.rax;)')
        if not re.search(terminal, body):
            errors.append(fn + ': successful block must terminate FD borrowing')
    body = re.sub(r'\s+', '', function_body(production_source(files['kernel/src/syscall/native_ipc.zig']), 'maybeHandoffToRecvWaiter'))
    if not body.endswith('if(!switchIpc(h,frame,0,0,0,target,sender_rax))returnnull;returnframe.rax;'):
        errors.append('successful IPC handoff must terminate state access')
    notify_handoff = re.sub(r'\s+', '', function_body(production_source(files['kernel/src/syscall/native_ipc.zig']), 'maybeHandoffNotify'))
    terminal_notify = ('if(!scheduler.handoffNotifyToReadyThreadGenerationWithRax(frame,target.thread_index,'
                       'target.thread_generation,status,h.before_current_thread_leave))returnnull;returnframe.rax;')
    if not notify_handoff.endswith(terminal_notify):
        errors.append('successful NOTIFY handoff must terminate FD borrowing')
    return errors


def pt_storage_boundary(files):
    source = production_source(files['kernel/src/memory/pt_storage.zig'])
    # Remove exactly the host fixture declaration, not everything following
    # it: a later helper is still production and can be called by Store.
    match = re.search(r'\bconst TestAllocator = struct\s*\{', source)
    if match:
        start = source.index('{', match.start())
        depth, end = 1, start + 1
        while depth and end < len(source):
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        if depth:
            return ['PT storage: unterminated host fixture']
        source = source[:match.start()] + ' ' * (end - match.start()) + source[end:]
    entry = production_source(files['kernel/src/boot/entry.zig'])
    errors = []
    # This guards the issuer plumbing, not detach/TLB ordering (which needs
    # runtime and MMIO fixtures). Masked strings cannot supply the contract.
    ensure = function_body(source, 'ensurePageChecked')
    release = function_body(source, 'releasePageDetached')
    teardown = function_body(source, 'freeDetached')
    reclaim = function_body(source, 'reclaimEmptyBanks')
    if re.search(r'\bTestAllocator\b', source):
        errors.append('PT storage: production references host allocator fixture')
    if ('const issuer = bank.allocator;' not in ensure or
            'issuer.acquire(issuer.context, 1)' not in ensure or
            'entry.allocation = raw.allocation;' not in ensure or
            '.allocation = raw.allocation' not in ensure):
        errors.append('PT storage: bank-affine original issuer/grant missing')
    if ('self.bankAt(slot / slots_per_bank).?.allocator' not in release or
            'allocator.release(allocator.context, entry.allocation)' not in release or
            'const allocator = bank.allocator;' not in reclaim or
            'const bank_grant = bank.allocation;' not in reclaim or
            'self.releasePageDetached(slot)' not in teardown or
            'allocator.release(allocator.context, bank_grant)' not in reclaim):
        errors.append('PT storage: detached return lost original issuer/grant')
    if ('@ptrFromInt' in source or '@intFromPtr' in source or
            re.search(r'fn (?:freeDetached|releasePageDetached)\([^)]*allocator:', source)):
        errors.append('PT storage: caller-selected return or pointer reconstruction')
    # Imports are string-masked; check the actual boot binding rather than a
    # comment or an unused adapter declaration.
    if not re.search(r'\.pt_allocator\s*=\s*@import\(\s*\)\.allocatorFor\(kernel_runtime.global_free_list,\s*\.pt_storage\)', entry):
        errors.append('PT boot allocator: owned mapping adapter missing')
    # Pair the masked-code binding with its actual import string, rather than
    # accepting any module which happens to export an allocator function.
    raw_entry = files['kernel/src/boot/entry.zig']
    bindings = list(re.finditer(r'\.pt_allocator\s*=\s*@import\(\s*\)\.allocatorFor\(kernel_runtime.global_free_list,\s*\.pt_storage\)', entry))
    if len(bindings) != 1 or not re.fullmatch(
            r'\.pt_allocator\s*=\s*@import\("\.\./memory/identity_allocator\.zig"\)\.allocatorFor\(kernel_runtime.global_free_list,\s*\.pt_storage\)',
            raw_entry[bindings[0].start():bindings[0].end()] if len(bindings) == 1 else ''):
        errors.append('PT boot allocator: wrong owned adapter import')
    return errors


def pt_lifecycle_boundary(files, census=None):
    """Scoped order guard plus actual callback tests; not a TLB proof alone."""
    vm = production_source(files['kernel/src/memory/user_vm.zig'])
    space = production_source(files['kernel/src/memory/address_space.zig'])
    store = production_source(files['kernel/src/memory/pt_storage.zig'])
    errors = []
    compact = lambda body: re.sub(r'\s+', '', body)
    for name in ('retireEmptyUserPtSlots', 'detachAndReleaseUserPtStorage', 'rollback'):
        body = function_body(vm, name)
        flush = body.find('h.flush_user_tlb_for_principal_range(')
        release = re.search(r'\.(?:releasePageDetached|freeDetached)\(', body)
        if flush < 0 or release is None or release.start() < flush:
            errors.append(name + ': return before synchronous ACK')
    batch = compact(function_body(vm, 'retireEmptyUserPtSlots'))
    detach = compact(function_body(vm, 'detachEmptyUserParents'))
    barrier = batch.find('h.flush_user_tlb_for_principal_range(')
    if batch.count('h.flush_user_tlb_for_principal_range(') != 1 or any(
            not 0 <= batch.find('detachEmptyUserParents(h,space,.' + level + ',') < barrier
            for level in ('pd', 'pdp')):
        errors.append('parent retirement: all levels must detach before one batch ACK')
    if 'releasePageDetached(' in detach or 'flush_user_tlb' in detach or 'parent.*=0;' not in detach:
        errors.append('parent retirement: detach helper must not return or ACK')
    finish = compact(function_body(vm[vm.index('const TablePathPreparation'):], 'finish'))
    if not finish.startswith('if(self.committed)return;'):
        errors.append('path rollback: published path treated as unpublished')
    publish = function_body(vm[vm.index('const TablePathPreparation'):], 'publish')
    if any(word in publish for word in ('ensurePage(', '.acquire(', 'orelse', 'catch')):
        errors.append('path publication: fallible operation after preflight')
    for level in ('pdp', 'pd', 'pt'):
        if not re.search(r'\b' + level + r'_page_used_len:\s*usize\b', space):
            errors.append(level + ': narrowed software high water')
    if re.search(r'(?:slots|candidates):\s*\[[^]]*\]u16|touched_slots:\s*\[\]u16|old_\w+_used:\s*u16', vm):
        errors.append('PT software retirement slot narrowed to u16')
    if re.search(r'\b(?:max_dynamic_(?:pdp|pd|pt)_pages|max_banks|max_slots)\b', vm + space + store):
        errors.append('PT arbitrary capacity reintroduced')
    reclaim = compact(function_body(store, 'reclaimEmptyBanks'))
    if ('constallocator=chunk.allocator;' not in reclaim or
            'constgrant=chunk.allocation;' not in reclaim or
            'allocator.release(allocator.context,grant)' not in reclaim):
        errors.append('PT chunk: original issuer/grant lost')
    if census is not None:
        actual = pt_lifecycle_census(files)
        if actual != census['references']:
            errors.append('PT ownership callers changed without census review')
    return errors


def pt_lifecycle_census(files):
    # Include aliases as well as calls; unlisted new ownership paths must be
    # reviewed for unpublished versus detach/ACK lifetime before acceptance.
    names = ('ensurePage', 'ensurePageChecked', 'releasePageDetached', 'freeDetached', 'reclaimEmptyBanks')
    return {path: dict(counts) for path, raw in sorted(files.items())
            if (counts := Counter(re.findall(r'\b(' + '|'.join(names) + r')\b', production_source(raw))))}


def without_profile_stages(code):
    """Drop comptime-gated lock_profile stage statements from whitespace-free
    code, then unwrap the bare blocks that only scoped their samples. Braces
    owned by if/while/defer/else are kept because they follow a token."""
    code = re.sub(r'const\w+=if\(comptimelock_profile\.enabled\)lock_profile\.stageBegin\(\)else\{\};', '', code)
    code = re.sub(r'if\(comptimelock_profile\.enabled\)lock_profile\.stageEnd\([^;]*\);', '', code)
    index = 0
    while index < len(code):
        if code[index] == '{' and index > 0 and code[index - 1] in ';{}':
            depth, end = 1, index + 1
            while depth and end < len(code):
                depth += (code[end] == '{') - (code[end] == '}')
                end += 1
            code = code[:index] + code[index + 1:end - 1] + code[end:]
            continue
        index += 1
    return code


def pending_data_return_boundary(files):
    """Pin this retirement protocol; not a proof of all VM writer locks."""
    errors = []
    vmo = production_source(files['kernel/src/state/vmo.zig'])
    types = production_source(files['kernel/src/state/types.zig'])
    syscalls_source = production_source(files['kernel/src/syscalls.zig'])
    outer = re.sub(r'\s+', '', function_body(syscalls_source, 'dispatchCompactSyscall'))
    dispatch = re.sub(r'\s+', '', function_body(syscalls_source, 'dispatchCompactSyscallBody'))
    if ('constresult=dispatchCompactSyscallBody(frame);' not in outer or
            outer.count('dispatchCompactSyscallBody(frame)') != 1):
        errors.append('pending drain: syscall wrapper must enter the reviewed K interval once')
    compact = lambda name: re.sub(r'\s+', '', function_body(vmo, name))
    for name in ('nativeVmoSlot', 'nativeVmoSlotConst', 'nativeCowTableSlot'):
        if not re.search(r'(?:slot|value)\.pending_data_return!=null\)returnnull;', compact(name)):
            errors.append(name + ': retiring owner exposed through normal accessor')
    for name in ('releaseNativeVmoWithFreeList', 'releaseNativeCowTable'):
        body = compact(name)
        if (not re.search(r'slot\.pending_data_return=(?:free_list|destination);'
                          r'_=@atomicRmw\(usize,&self\.pending_data_returns,\.Add,1,\.release\);return;', body)):
            errors.append(name + ': failed destination or atomic retirement publication missing')
    release = compact('releaseNativeVmoWithFreeList')
    drain = compact('drainPendingDataReturns')
    if ('if(vmoDataOwnerMatches(slot,expected_owner))@TypeOf(self.*).releaseNativeVmoOwnedPages(slot,free_list);' not in release or
            'if(!vmoDataOwnerMatches(slot,expected_owner))continue;@TypeOf(self.*).releaseNativeVmoOwnedPages(slot,destination);' not in drain):
        errors.append('VMO final return: expected owner not checked before returning RAM')
    if ('snapshotVmoBackingPagesChecked(' not in compact('backingReturnUnfinished') or
            'catchreturntrue;' not in compact('backingReturnUnfinished')):
        errors.append('final return: invalid backing snapshot treated as empty')
    if compact('vmoDataOwnerMatches') != ('return!vmoHasOwnedDataStore(slot)or'
            'types.vmoBackingPageStoreOwnerMatches(slot.page_store_start,slot.page_count,slot.page_store_owner,expected_owner);'):
        errors.append('VMO final return: expected token validation bypassed')
    token_check = re.sub(r'\s+', '', function_body(types, 'vmoBackingPageStoreOwnerMatches'))
    if ('constvalid=recorded==expectedandstore.matches(start,expected);' not in token_check or
            'ownershipFault(4,start,page_count,0,expected);' not in token_check or not token_check.endswith('returnfalse;')):
        errors.append('VMO final return: foreign token admitted or missing failure diagnostic')
    if ('slot.pending_data_return=null;self.clearNativeVmoSlot(slot);'
            '_=@atomicRmw(usize,&self.pending_data_returns,.Sub,1,.acq_rel);' not in drain or
            'if(!parent.isNull())self.releaseNativeVmoWithFreeList(parent,destination);' not in drain or
            'self.native_cow_tables.remove(slot,null);_=@atomicRmw(usize,&self.pending_data_returns,.Sub,1,.acq_rel);' not in drain):
        errors.append('final return: successful reset, atomic removal or parent edge drop missing')
    # The aggregate is shared by disjoint writer domains. A plain field read
    # cannot be substituted even when a caller holds one of those domains.
    for path, raw in files.items():
        source = production_source(raw)
        atomic = re.sub(r'@atomic(?:Rmw|Load)\([^;]+?&self\.pending_data_returns[^;]+?\)', '', source)
        if re.search(r'\.pending_data_returns\b', atomic):
            errors.append(path + ': non-atomic cross-domain pending hint access')
    if '@atomicLoad(usize,&self.pending_data_returns,.acquire)!=0' not in compact('hasPendingDataReturns'):
        errors.append('pending hint: acquire check missing')
    runtime = ('if(lock_held){constretire_sample=if(comptimelock_profile.enabled)lock_profile.stageBegin()else{};@import().retryRetiredAddressSpaces();state.retryInvocationStorage();state.retryReferenceStorage();native_ipc_syscalls.drainCancellations(h,state);capsule_syscalls.drainDeviceMemoryRetirements(state);'
               'ram_cache_syscalls.drain(state,h.free_list);completeDeferredIrqFdWaitersLocked(h);fd_syscalls.completePersistentWaiters(h,state);if(state.hasPendingDataReturns()){'
               '@import().lockAllVmTransactions();state.drainPendingDataReturns();'
               '@import().unlockAllVmTransactions();}if(comptimelock_profile.enabled)lock_profile.stageEnd(.retire,retire_sample);kernel_state_lock.unlock();'
               'handOffIrqFdWaitersAfterRelease();}')
    # The migration oracle executes after handler defers under the same K
    # interval. Admit exactly its all-AS comparison envelope; do not let it
    # hide an unlock or a second pending-return call from the guard.
    oracle_prefix = 'if(comptime@import().enabled){'
    start = dispatch.find(oracle_prefix)
    if start >= 0:
        depth = 1
        end = start + len(oracle_prefix)
        while depth and end < len(dispatch):
            depth += (dispatch[end] == '{') - (dispatch[end] == '}')
            end += 1
        oracle = dispatch[start:end]
        sequence = ('@import().lockAllVmTransactions();', '@import().check(state)', '@import().unlockAllVmTransactions();')
        positions = [oracle.find(token) for token in sequence]
        if min(positions) < 0 or positions != sorted(positions) or any(oracle.count(token) != 1 for token in sequence) or 'kernel_state_lock.' in oracle or 'drainPendingDataReturns' in oracle:
            errors.append('reference oracle: outer K/all-AS comparison boundary bypassed')
        dispatch = dispatch[:start] + dispatch[end:]
    # Lock-profile stages compile out of production and only time the reviewed
    # calls, so compare the interval without them; every call, its order, and
    # every conditional scope remain exact.
    runtime = without_profile_stages(runtime)
    dispatch = without_profile_stages(dispatch)
    if runtime not in dispatch or not dispatch.index('defer{') < dispatch.index(runtime):
        errors.append('pending drain: outer KernelState/all-AS/sharedVM boundary bypassed')
    calls = sum(production_source(raw).count('.drainPendingDataReturns(') for raw in files.values())
    if calls != 1:
        errors.append('pending drain: unreviewed production call or alias')
    references = {path: len(re.findall(r'\bdrainPendingDataReturns\b', production_source(raw)))
                  for path, raw in files.items() if re.search(r'\bdrainPendingDataReturns\b', production_source(raw))}
    if references != {'kernel/src/kernel.zig': 2, 'kernel/src/state/vmo.zig': 1, 'kernel/src/syscalls.zig': 1}:
        errors.append('pending drain: unreviewed production reference/alias')
    # Import names are masked above; bind the actual code spans to the VM
    # lock provider so a same-named function from a different module fails.
    raw_dispatch = files['kernel/src/syscalls.zig']
    masked_dispatch = production_source(raw_dispatch)
    for match in re.finditer(r'@import\(\s*\)\.(?:un)?lockAllVmTransactions\(\)', masked_dispatch):
        if not re.fullmatch(r'@import\("memory/user_vm\.zig"\)\.(?:un)?lockAllVmTransactions\(\)',
                            raw_dispatch[match.start():match.end()]):
            errors.append('pending drain: wrong VM lock provider')
    census = re.sub(r'\s+', '', function_body(types, 'vmObjectBackingStoreBreakdown'))
    if '(vmo.ref_count==0andvmo.pending_data_return==null)' not in census:
        errors.append('backing census: retained final owner counted as invalid')
    return errors



# These are lifetime-bearing accesses, including aliases and registration, not
# just calls. Exact reviewed bodies make a newly inserted lock drop/escape fail
# even when the accessor counts stay unchanged. This is a scoped source tripwire,
# not a Zig-wide alias/lifetime proof or lock-rank checker (lock ordering).
PROCESS_LIFETIME_NAMES = (
    r'createProcessDescriptor\w*|ensureProcessDescriptor|clearPrincipalTablesForReuse|'
    r'clearPrincipalState|initPrincipalState|initDynamicPrincipalState|resetProcessRuntimeTables|'
    r'getVmaTable(?:Const)?|vmaTableForProcessIndex(?:Const)?|'
    r'getFdTable(?:Const)?|fdTableForProcessIndex(?:Const)?|fdTableForActiveProcess(?:Const)?|'
    r'fdEntryConst|fdPayloadWithRights(?:Const)?|'
    r'prepare(?:Fixed\w*Mmap|MunmapRange|Mremap)\w*|(?:commit|discard)(?:FixedMmap|Munmap|Mremap)Prepared|'
    r'process_timer\.(?:get|set|clear)|process_timers\.(?:take|reserve|release|set|cancel)|'
    r'takeExpiredProcessSignalTimer|clearProcessSignalTimer|setProcessSignalTimer|'
    r'getProcessSignalTimer|deliverSignal|deliverSignalLocked|copyPendingSignalHint|completeProcessSignalTimerLocked|'
    r'completeProcessSignalTimersFromInterrupt|quiescePrincipalForLifecycle')


def process_lifetime_census(files):
    import hashlib
    result = {}
    for path, original in sorted(files.items()):
        if not path.startswith('kernel/src/'):
            continue
        source = production_source(original)
        counts = dict(sorted(Counter(re.findall(r'\b(?:' + PROCESS_LIFETIME_NAMES + r')\b', source)).items()))
        if not counts:
            continue
        bodies = {}
        # State accessors inherit K or VM from their caller. Freeze the outside
        # borrower body and state admission/reset chain where that contract lives.
        for match in re.finditer(r'^\s*(?:pub\s+)?(?:(?:inline|noinline)\s+)?fn\s+(\w+)\s*\(', source, re.M):
            name = match[1]
            body = function_body(source, name)
            if not re.search(r'\b(?:' + PROCESS_LIFETIME_NAMES + r')\b', body):
                continue
            if '/state/' in path and name not in ('clearPrincipalTablesForReuse', 'clearPrincipalState',
                    'createProcessDescriptorBelowChecked', 'ensureProcessDescriptor', 'initPrincipalState',
                    'initDynamicPrincipalState', 'resetProcessRuntimeTables', 'releaseProcessTimer', 'recordFor'):
                continue
            bodies[name] = hashlib.sha256(re.sub(r'\s+', '', body).encode()).hexdigest()
        result[path] = dict(mentions=counts, reviewed_bodies=bodies)
    return result


def process_lifetime_boundary(files):
    errors = []
    expected = json.loads((ROOT / 'tests/process-lifetime-caller-census.json').read_text())
    if process_lifetime_census(files) != expected:
        errors.append('process lifetime: borrower/reuse/timer caller or reviewed lock interval changed')
    def body(path, name):
        return re.sub(r'\s+', '', function_body(production_source(files['kernel/src/' + path]), name))
    irq = body('syscalls.zig', 'completeProcessSignalTimersFromInterrupt')
    sequence = ('tlbShootdownActiveOnCurrentCpu()', 'if(!kernel_state_lock.tryLock())return;',
                'deferkernel_state_lock.unlock();', 'state.process_timers.take(now_tick)',
                'completeProcessSignalTimerLocked(state,expired)')
    positions = [irq.find(part) for part in sequence]
    if min(positions) < 0 or positions != sorted(positions) or irq.count('kernel_state_lock.unlock()') != 1:
        errors.append('process timer: consumption/delivery must share K tryLock interval')
    for path, name in (('syscall/process.zig', 'cleanupProcess'),
                       ('boot/entry.zig', 'teardownExitedProcess'),
                       ('boot/entry.zig', 'teardownFaultedProcess')):
        teardown = body(path, name)
        sequence = ('scheduler.detachPrincipalIpcBindings(principal);',
                    '.drainIpcCancellations(', 'user_vm.clearUserAddressSpace(principal);')
        positions = [teardown.find(part) for part in sequence]
        if min(positions) < 0 or positions != sorted(positions):
            errors.append(name + ': IPC binding withdrawal/drain must precede AS destruction')
    lifecycle = body('syscall/process.zig', 'quiescePrincipalForLifecycle')
    required = ('reacquire_lock.run(reacquire_lock.context);'
                'constdescriptor=state.processDescriptor(target)orelsereturnfalse;'
                'if(descriptor.generation!=generation)returnfalse;'
                '_=scheduler.stopPrincipalThreads(target);')
    if required not in lifecycle:
        errors.append('process lifecycle: generation must be checked immediately after K reacquisition')
    record = body('state/process_timer.zig', 'recordFor')
    if 'if(descriptor.generation!=generation)returnnull;' not in record:
        errors.append('process timer: descriptor generation admission missing')
    for name in ('createProcessDescriptorBelowChecked', 'ensureProcessDescriptor'):
        code = body('state/process.zig', name)
        if not (0 <= code.find('process_timers.reserve(') < code.find('.active=true')):
            errors.append(name + ': timer reservation must precede active publication')
    for name in ('markProcessFaulted', 'markProcessExited', 'removeProcessDescriptor'):
        code = body('state/process.zig', name)
        if not (0 <= code.find('releaseProcessTimer(') < code.find('lockDescriptors(')):
            errors.append(name + ': timer withdrawal must precede descriptor retirement')
    delivery = body('scheduler_connection.zig', 'deliverSignal')
    if 'if(process_generation==0ordescriptor.generation!=process_generation)returnnull;' not in delivery:
        errors.append('process delivery: generation admission missing')
    hint = body('scheduler_connection.zig', 'deliverSignalLocked')
    if ('descriptor.generation==expectedanddescriptor.signal_pending_hint_va!=0' not in hint or
            hint.find('scheduler_state.thread_table.unlock();') > hint.find('copyPendingSignalHint(')):
        errors.append('process hint: identity check and T release before copyout required')
    for path, name, owner, index, deferred in (
            ('syscall/process.zig', 'cleanupProcess', 'principal', 'process_index', False),
            ('syscall/process.zig', 'cloneCurrentProcessForFork', 'child', 'child_index', True),
            ('boot/entry.zig', 'teardownFaultedProcess', 'principal', 'process_index', False),
            ('boot/entry.zig', 'teardownExitedProcess', 'principal', 'process_index', False)):
        code = body(path, name)
        lock = code.find('user_vm.lockVmTransaction(' + owner + ')')
        reset = code.find('.resetProcessRuntimeTables(' + index + ')')
        unlock = code.find(('defer' if deferred else '') + 'user_vm.unlockVmTransaction(' + owner + ')')
        if min(lock, reset, unlock) < 0 or not (lock < unlock < reset if deferred else lock < reset < unlock):
            errors.append(name + ': runtime VMA reset must remain inside A/S transaction')
    for name in ('installProcessMapLocked', 'unmapFromProcess', 'mapIntoProcess', 'mapBatchIntoProcess'):
        code = body('syscall/process.zig', name)
        if any(word in code for word in ('before_current_thread_leave', 'reacquire_kernel_state_lock', 'kernel_state_lock.unlock')):
            errors.append(name + ': noncurrent prepared operation/borrow crosses K')
    return errors

def main():
    sources = {path: (ROOT / 'kernel/src' / path).read_text() for path in NAMES}
    errors = check(sources)
    if errors:
        raise SystemExit('\n'.join(errors))
    # Every scoped rule is also exercised with an in-memory violating source;
    # explanations cannot satisfy a check because comments/strings are masked.
    violations = (
        ('scheduler_connection.zig', 'ctx.generation = slot.generation;', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'group.live == 0', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'cpu.current_context == ctx', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'cpu.current_entity == node', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'cpu.runqueue.root == node', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'node.linked_tree != null', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'retireAllocation(ThreadContext, contexts.ptr)', 'bypass_lifetime'),
        ('scheduler_connection.zig', 'retireAllocation(scheduler_runqueue.Node, entities.ptr)', 'bypass_lifetime'),

        ('state/process.zig', 'descriptors.commit()', 'bypass_commit()'),
        ('state/process.zig', 'descriptors.rollback()', 'bypass_rollback()'),
        ('scheduler_connection.zig', 'ThreadGrowth.reserve(', 'allocKernelSlice('),
        ('scheduler_connection.zig', 'transaction.rollback()', 'bypass_rollback()'),
        ('scheduler_connection.zig', 'scheduler_runqueue.Node, capacity - old_capacity, .stable',
         'scheduler_runqueue.Node, capacity - old_capacity, .replaceable'),
        ('scheduler_connection.zig', 'ThreadContext, capacity - old_capacity, .stable',
         'ThreadContext, capacity - old_capacity, .replaceable'),
        ('scheduler_connection.zig', 'contexts: []const *ThreadContext', 'contexts: []*ThreadContext'),
        ('scheduler_connection.zig', '@atomicLoad(*const ThreadDirectory, &scheduler_state.thread_table.directory, .acquire)',
         'scheduler_state.thread_table.directory'),
        ('scheduler_connection.zig', '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release)',
         'scheduler_state.thread_table.directory = draft.directory'),
        ('scheduler_connection.zig', 'fn threadDirectory() *const ThreadDirectory {',
         'fn threadDirectory() *const ThreadDirectory { _ = scheduler_state.thread_table.directory;'),
        ('scheduler_connection.zig', '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release);',
         '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release); scheduler_state.thread_table.directory = draft.directory;'),
        ('scheduler_connection.zig', '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release);',
         '@atomicStore(*const ThreadDirectory, &scheduler_state.thread_table.directory, draft.directory, .release); draft.directory.* = undefined;'),
        ('scheduler_connection.zig', 'fn threadContextLocked(thread_index: usize) ?*ThreadContext {',
         'fn threadContextLocked(thread_index: usize) ?*ThreadContext { _ = threadDirectory();'),
        ('scheduler_connection.zig', 'defer unlockThreadDirectory();', ''),
        ('scheduler_connection.zig', 'const old_capacity = snapshotThreadCapacityAndRetryReturns();',
         'const old_capacity = threadCapacity();'),
        ('scheduler_connection.zig', 'unlockThreadDirectory();\n    queueReclaimGroupLocked(old_capacity);\n    scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion()',
         'scheduler_state.thread_table.storage.reclaimRetiredAfterBorrowExclusion()'),
        ('scheduler_connection.zig', 'scheduler_state.thread_table.directory_lock.lock();',
         'scheduler_state.thread_table.directory_lock.lock(); scheduler_state.thread_table.lock();'),
        ('scheduler_connection.zig', 'return threadContextLocked(thread_index);',
         'return &threadDirectory().hot_states[thread_index];'),
        ('scheduler_connection.zig', 'defer scheduler_state.thread_table.unlock();\n        transaction.rollback()',
         'transaction.rollback()'),
        ('memory/identity_allocator.zig', 'grant.physical.toInt(), grant.pages', '0, grant.physical.toInt(), grant.pages'),
        ('memory/identity_allocator.zig', 'layout.identitySlice(', '@ptrFromInt('),
        ('memory/free_page_list.zig', 'return (try self.popContiguousOwnedInRange(', 'return (try self.popContiguousBelow('),
        ('state/memory.zig', 'fn initializeStorageInPlace(', 'pub fn initializeStorageInPlace('),
        ('kernel.zig', 'pub const KernelState = struct {',
         'pub const KernelState = struct { pub const resetStorageInPlace = memory_state.initializeStorageInPlace;'),
    )
    for path, old, new in violations:
        changed = dict(sources)
        assert old in changed[path], (path, old)
        if path == 'state/process.zig':
            start = changed[path].index('pub fn ensureProcessCapacity(')
            changed[path] = changed[path][:start] + changed[path][start:].replace(old, new, 1)
        else:
            changed[path] = changed[path].replace(old, new, 1)
        assert check(changed), (path, old)
    for addition in ('\nfn unsafeThreadRead() void { _ = threadDirectory(); }\n',
                     '\nconst unsafeThreadAlias = getThreadHotState;\n'):
        changed = dict(sources)
        changed['scheduler_connection.zig'] += addition
        assert check(changed), 'new metadata borrower / alias escaped census'
    commented = dict(sources)
    commented['memory/identity_allocator.zig'] += '\n// @ptrFromInt and @intFromPtr forbidden here.\n'
    assert not check(commented)
    # Constructor callsite audit tripwire, NOT a proof that a live object has
    # no backing/borrowers. New uses must be reviewed rather than silently
    # widening the fresh-storage precondition to an arbitrary reset contract.
    expected = {
        'kernel/src/kernel.zig': Counter(initPhase1InPlace=3),
        'kernel/src/boot/entry.zig': Counter(initFromDetectedRegionsInPlace=1),
        # Frontend, dedicated direct and K/poll exclusion fixtures each own
        # fresh heap state and detach/drain nodes before pool teardown.
        'kernel/src/scheduler_connection.zig': Counter(initFromDetectedRegionsInPlace=10),
        # Heap fixture owns runtime and drains rejected physical returns at teardown.
        'kernel/src/state/fd_capability.zig': Counter(initFromDetectedRegionsInPlace=1),
        # Lifecycle/timer/K-drop and IPC idle-drain fixtures each own fresh heap state.
        'kernel/src/syscalls.zig': Counter(initFromDetectedRegionsInPlace=5),
        'kernel/src/syscall/process.zig': Counter(initFromDetectedRegionsInPlace=1),
        # Snapshot, retained-set rollback, HUP, and parked-token fixtures each
        # allocate and destroy their own state, including source observers.
        'kernel/src/syscall/fd.zig': Counter(initFromDetectedRegionsInPlace=4),
        # withdrawal contract owner-cleanup and FD-alias tests each allocate a fresh KernelState
        # and destroy it at fixture end; neither resets a live owner. Include
        # the pre-existing VT-d failed-close fixture in its exact call census.
        'kernel/src/vtd.zig': Counter(initFromDetectedRegionsInPlace=3),
        'kernel/src/amd_iommu.zig': Counter(initFromDetectedRegionsInPlace=2),
        # MmioRetirementFixture.start allocates a fresh KernelState; end destroys that same
        # allocation. It never uses the constructor to reset a live owner.
        'kernel/src/mmio_overlay_test.zig': Counter(initFromDetectedRegionsInPlace=2),
        'tests/kernel_state.zig': Counter(initFromDetectedRegionsInPlace=2),
        'tests/fd_ipc_minimal.zig': Counter(initFromDetectedRegionsInPlace=1),
    }
    def census(files):
        result = {}
        for path, source in files.items():
            counts = Counter(re.findall(r'\.(initPhase1InPlace|initFromDetectedRegionsInPlace|initFromDetectedRegions|initPhase1)\s*\(', code_only(source)))
            if counts:
                result[path] = counts
        return result
    files = {str(path.relative_to(ROOT)): path.read_text() for directory in ('kernel/src', 'tests', 'bootloader/limine')
             for path in (ROOT / directory).rglob('*.zig')}
    assert not thread_identity_boundary(files), thread_identity_boundary(files)
    for injected in ('const alias = scheduler.releaseThread;',
                     'fn bad() void { _ = scheduler.releaseThread(4); }',
                     'const hooks = .{ .release = scheduler.releaseThread };'):
        changed = dict(files)
        changed['kernel/src/late_identity.zig'] = injected
        assert thread_identity_boundary(changed), injected
    assert not process_lifetime_boundary(files), process_lifetime_boundary(files)
    for path, old, new in (
            ('kernel/src/syscalls.zig',
             'while (state.process_timers.take(now_tick))',
             'kernel_state_lock.unlock(); while (state.process_timers.take(now_tick))'),
            ('kernel/src/syscall/process.zig',
             'state.resetProcessRuntimeTables(process_index);',
             'user_vm.unlockVmTransaction(principal); state.resetProcessRuntimeTables(process_index);'),
            ('kernel/src/syscall/process.zig',
             'state.commitFixedMmapPrepared(&prepared, free_list);',
             'h.before_current_thread_leave.?.run(h.before_current_thread_leave.?.context); state.commitFixedMmapPrepared(&prepared, free_list);')):
        changed = dict(files)
        assert old in changed[path]
        changed[path] = changed[path].replace(old, new, 1)
        assert process_lifetime_boundary(changed), old
    for path in ('kernel/src/syscall/process.zig', 'kernel/src/boot/entry.zig'):
        for replacement in ('', 'user_vm.clearUserAddressSpace(principal); scheduler.detachPrincipalIpcBindings(principal);'):
            changed = dict(files)
            changed[path] = changed[path].replace('scheduler.detachPrincipalIpcBindings(principal);', replacement)
            assert process_lifetime_boundary(changed), 'IPC teardown withdrawal/order bypass'
        changed = dict(files)
        changed[path] = re.sub(r'[\w.]+\.drainIpcCancellations\([^;]+;', '', changed[path])
        assert process_lifetime_boundary(changed), 'IPC teardown drain bypass'
    changed = dict(files)
    changed['kernel/src/late_process_reuse.zig'] = 'const alias = kernel.KernelState.ensureProcessDescriptor;'
    assert process_lifetime_boundary(changed), 'fresh bootstrap API alias escaped live reuse census'
    changed = dict(files)
    changed['kernel/src/late_process_timer.zig'] = 'fn unlocked() void { process_timer.clear(state, principal, generation); }'
    assert process_lifetime_boundary(changed), 'process timer caller escaped K interval census'

    assert not pt_storage_boundary(files), pt_storage_boundary(files)
    pt_census = json.loads((ROOT / 'tests/pt-storage-caller-census.json').read_text())
    assert not pt_lifecycle_boundary(files, pt_census), pt_lifecycle_boundary(files, pt_census)
    for path, old, new in (
            ('kernel/src/memory/address_space.zig', 'pt_page_used_len: usize', 'pt_page_used_len: u16'),
            ('kernel/src/memory/user_vm.zig', 'touched_slots: []usize', 'touched_slots: []u16'),
            ('kernel/src/memory/user_vm.zig', 'if (self.committed) return;', 'if (false) return;'),
            ('kernel/src/memory/pt_storage.zig', 'const grant = chunk.allocation;', 'const grant = guessed_grant;')):
        changed = dict(files)
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert pt_lifecycle_boundary(changed, pt_census), old
    for name in ('retireEmptyUserPtSlots', 'detachAndReleaseUserPtStorage', 'rollback'):
        changed = dict(files)
        path = 'kernel/src/memory/user_vm.zig'
        body = function_body(changed[path], name)
        altered = body.replace('h.flush_user_tlb_for_principal_range(', 'omitted_ack(', 1)
        assert altered != body
        changed[path] = changed[path].replace(body, altered, 1)
        assert pt_lifecycle_boundary(changed, pt_census), name
    changed = dict(files)
    changed['kernel/src/pt_return_bypass.zig'] = 'const bypass = store.releasePageDetached;'
    assert pt_lifecycle_boundary(changed, pt_census)

    assert not pending_data_return_boundary(files), pending_data_return_boundary(files)
    for replacement in ('kernel_state_lock.unlock(); @import("state/reference_check.zig").check(state)', '@import("state/reference_check.zig").ignore(state)'):
        changed = dict(files)
        changed['kernel/src/syscalls.zig'] = changed['kernel/src/syscalls.zig'].replace('@import("state/reference_check.zig").check(state)', replacement)
        assert pending_data_return_boundary(changed), 'reference oracle lock/check bypass'
    pending_violations = (
        ('kernel/src/state/vmo.zig', 'if (slot.pending_data_return != null) return null;', ''),
        ('kernel/src/state/vmo.zig', 'slot.pending_data_return = free_list;', 'slot.pending_data_return = null;'),
        ('kernel/src/state/vmo.zig', '.Add, 1, .release', '.Add, 1, .monotonic'),
        ('kernel/src/state/vmo.zig', '@atomicLoad(usize, &self.pending_data_returns, .acquire)', 'self.pending_data_returns'),
        ('kernel/src/state/vmo.zig',
         'types.snapshotVmoBackingPagesChecked(start, count, owner, 0, count, &remaining) catch return true;',
         'types.snapshotVmoBackingPagesChecked(start, count, owner, 0, count, &remaining) catch return false;'),
        ('kernel/src/state/vmo.zig', 'if (vmoDataOwnerMatches(slot, expected_owner))', 'if (true)'),
        ('kernel/src/state/types.zig', 'recorded == expected', 'recorded != 0'),
        ('kernel/src/syscalls.zig', 'if (lock_held) {', 'if (true) {'),
        ('kernel/src/syscalls.zig', '.lockAllVmTransactions();', '.ignoreVmTransactions();'),
        ('kernel/src/syscalls.zig', 'state.retryInvocationStorage();', ''),
        ('kernel/src/state/types.zig', '(vmo.ref_count == 0 and vmo.pending_data_return == null)', 'vmo.ref_count == 0'),
    )
    for path, old, new in pending_violations:
        changed = dict(files)
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert pending_data_return_boundary(changed), old
    changed = dict(files)
    changed['kernel/src/late_pending_bypass.zig'] = 'fn bypass(s: anytype) void { s.drainPendingDataReturns(); }'
    assert pending_data_return_boundary(changed)
    changed = dict(files)
    changed['kernel/src/late_pending_bypass.zig'] = 'const escaped = state.drainPendingDataReturns;'
    assert pending_data_return_boundary(changed)
    changed = dict(files)
    changed['kernel/src/late_pending_bypass.zig'] = 'fn bypass(s: anytype) void { s . drainPendingDataReturns ( ); }'
    assert pending_data_return_boundary(changed)
    changed = dict(files)
    changed['kernel/src/syscalls.zig'] = changed['kernel/src/syscalls.zig'].replace('@import("memory/user_vm.zig").lockAllVmTransactions()', '@import("memory/fake_vm.zig").lockAllVmTransactions()', 1)
    assert pending_data_return_boundary(changed)
    for old, new in (
            ('const issuer = bank.allocator;', 'const issuer = allocator;'),
            ('entry.allocation = raw.allocation;', 'entry.allocation = undefined;'),
            ('allocator.release(allocator.context, entry.allocation)', 'allocator.release(allocator.context, guessed_grant)'),
            ('allocator.release(allocator.context, bank_grant)', 'allocator.release(allocator.context, guessed_grant)'),
            ('pub fn freeDetached(self: *Store)', 'pub fn freeDetached(self: *Store, allocator: Allocator)')):
        changed = dict(files)
        path = 'kernel/src/memory/pt_storage.zig'
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert pt_storage_boundary(changed), old
    changed = dict(files)
    path = 'kernel/src/memory/pt_storage.zig'
    changed[path] += '\nfn latePtBypass(value: usize) *u8 { return @ptrFromInt(value); }\n'
    changed[path] = changed[path].replace('const issuer = bank.allocator;', 'const issuer = bank.allocator; _ = latePtBypass(4096);', 1)
    assert pt_storage_boundary(changed)
    for alias in ('const escapedFixture = TestAllocator;',
                  'const escapedAcquire = TestAllocator.allocate;'):
        changed = dict(files)
        changed['kernel/src/memory/pt_storage.zig'] += '\n' + alias + '\n'
        assert pt_storage_boundary(changed)
    changed = dict(files)
    path = 'kernel/src/boot/entry.zig'
    changed[path] = changed[path].replace('../memory/identity_allocator.zig', '../memory/raw_allocator.zig')
    assert pt_storage_boundary(changed)
    def legacy_endpoint_boundary(sources):
        # This is the unreachable principal-route mechanism, not native IPC's
        # independently owned IpcEndpoint/IpcChannel objects or FD waiters.
        forbidden = re.compile(r'\b(?:EndpointRoute|EndpointTable|PublishedEndpointTable|'
                               r'endpoint_tables(?:_extra)?|published_service_endpoints|endpoint_generation|'
                               r'installSpawnParentEndpoint|installServiceEndpointForActiveProcesses|'
                               r'publishServiceEndpoint|unpublishServiceEndpointsForTarget|'
                               r'endpointTargetFor(?:KnownActiveOwner)?|scrubEndpointTargets)\b')
        return [path for path, source in sources.items()
                if forbidden.search(production_source(source))]
    assert not legacy_endpoint_boundary(files), legacy_endpoint_boundary(files)
    for injection in ('pub const EndpointRoute = struct {};',
                      'fn bypass(s: anytype) void { _ = s.endpoint_tables_extra; }',
                      'const alias = state.installSpawnParentEndpoint;'):
        changed = dict(files)
        changed['kernel/src/legacy_endpoint_bypass.zig'] = injection
        assert legacy_endpoint_boundary(changed)
    assert not as_directory_boundary(files), as_directory_boundary(files)
    assert not vma_directory_boundary(files), vma_directory_boundary(files)
    assert not descriptor_boundary(files), descriptor_boundary(files)
    changed = dict(files)
    path = 'kernel/src/state/process.zig'
    original = function_body(changed[path], 'recordPrincipalSyscall')
    altered = original.replace('const flags = lockDescriptors(self);', 'const flags = false;')
    assert altered != original
    changed[path] = changed[path].replace(original, altered, 1)
    assert descriptor_boundary(changed), 'counter update without descriptor exclusion'
    assert not fd_storage_boundary(files), fd_storage_boundary(files)
    for path, old, new in (
            ('kernel/src/state/process.zig', 'while (self.fd_tables_extra.live < extra_count)', 'while (false)'),
            ('kernel/src/state/process.zig', '@atomicStore(usize, &self.fd_process_capacity, capacity, .seq_cst)', 'skip_bounds()'),
            ('kernel/src/state/types.zig', 'self.entries.allocate(allocator, leaf,', 'skip_preparation('),
            ('kernel/src/state/types.zig', '@atomicStore(usize, &self.capacity, target, .release);', 'self.capacity = minimum;'),
            ('kernel/src/state/process.zig', 'self.fd_tables_extra.reclaim()', 'skip_reclaim()'),
            ('kernel/src/syscall/lock_policy.zig', 'if (nr == runtime_abi.syscall_getpid', 'if (nr == fd_abi.syscall_fd_close or nr == runtime_abi.syscall_getpid')):
        changed = dict(files)
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert fd_storage_boundary(changed), old
    changed = dict(files)
    changed['kernel/src/fd_raw_reader.zig'] = 'fn bypass(s: anytype) void { _ = s.fd_tables_extra; }'
    assert fd_storage_boundary(changed)
    changed = dict(files)
    changed['kernel/src/fd_lockless_reader.zig'] = 'fn bypass(s: anytype) void { _ = s.getFdTable(.Init); }'
    assert fd_storage_boundary(changed)
    changed = dict(files)
    changed['kernel/src/fd_payload_reader.zig'] = 'fn bypass(s: anytype) void { _ = s.fdPayloadWithRightsConst(.Init, 0, .{}); }'
    assert fd_storage_boundary(changed)
    changed = dict(files)
    path = 'kernel/src/state/vma.zig'
    original = function_body(changed[path], 'releaseFdTableForProcessIndexWithFreeList')
    call = 'self.releaseFdStorage(table, free_list);'
    altered = original.replace(call, '').replace('var fd: usize = 0;', call + '\n    var fd: usize = 0;')
    assert altered != original
    changed[path] = changed[path].replace(original, altered, 1)
    assert fd_storage_boundary(changed)
    changed = dict(files)
    path = 'kernel/src/syscall/native_ipc.zig'
    old = 'h.before_current_thread_leave)) {\n        return frame.rax;'
    assert old in changed[path]
    changed[path] = changed[path].replace(old, 'h.before_current_thread_leave)) {\n        state.cancelFdWaitGroup(group, wait_token); return frame.rax;', 1)
    assert fd_storage_boundary(changed)
    for path, fn in (('kernel/src/syscall/fd.zig', 'fdWaitMany'),
                     ('kernel/src/syscall/native_ipc.zig', 'recvWaitArgs'),
                     ('kernel/src/syscall/native_ipc.zig', 'callSync'),
                     ('kernel/src/syscall/native_ipc.zig', 'notifyWait'),
                     ('kernel/src/syscall/native_ipc.zig', 'maybeHandoffToRecvWaiter'),
                     ('kernel/src/syscall/native_ipc.zig', 'maybeHandoffNotify')):
        changed = dict(files)
        original = function_body(changed[path], fn)
        position = original.rfind('return frame.rax;')
        assert position >= 0
        altered = original[:position] + '_ = h.state.fd_objects[0]; ' + original[position:]
        changed[path] = changed[path].replace(original, altered, 1)
        assert fd_storage_boundary(changed), fn
    changed = dict(files)
    path = 'kernel/src/state/process.zig'
    changed[path] += '\nfn bypassInlineDescriptor(self: anytype, index: usize) bool { return self.process_descriptors[index].active; }\n'
    assert descriptor_boundary(changed)
    for old, new in (
            ('pub fn processCapacitySnapshot(self: anytype) usize {',
             'pub fn processCapacitySnapshot(self: anytype) usize { return self.process_capacity;'),
            ('pub fn processDescriptor(self: anytype, principal: PrincipalId) ?ProcessDescriptor',
             'pub fn processDescriptor(self: anytype, principal: PrincipalId) ?*const ProcessDescriptor'),
            ('fn descriptorSlotLocked(', 'pub fn descriptorSlotLocked('),
            ('"pushfq; pop %[flags]; cli"', '"pushfq; pop %[flags]"'),
            ('self.descriptor_storage.reclaimRetiredAfterBorrowExclusion()', 'self.process_storage.reclaimRetiredAfterBorrowExclusion()'),
            ('@cmpxchgStrong(u8, &mutable.descriptor_lock', '@cmpxchgWeak(u8, &mutable.descriptor_lock')):
        changed = dict(files)
        path = 'kernel/src/state/process.zig'
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert descriptor_boundary(changed), old
    for old, new in (
            ('defer unlockVmaDirectory(self, interrupts);', ''),
            ('return vmaTableForProcessIndex(@constCast(self), index);', 'return &self.vma_tables[0];'),
            ('extra >= self.vma_tables_extra.len', 'index >= self.process_capacity'),
            ('bodies.commitAdditionalStable()', 'bodies.commit()'),
            ('self.vma_directory_storage.reclaimRetiredAfterBorrowExclusion()', 'self.process_storage.reclaimRetiredAfterBorrowExclusion()'),
            ('"pushfq; pop %[flags]; cli"', '"pushfq; pop %[flags]"'),
            ('directory.commit()', 'directory.commit(); return false')):
        changed = dict(files)
        path = 'kernel/src/state/process.zig'
        assert old in changed[path], old
        changed[path] = new.join(changed[path].rsplit(old, 1))
        assert vma_directory_boundary(changed), old
    for old, new in (
            ('defer self.unlockDirectory(restore_interrupts);', ''),
            ('&self.body_storage,', '&self.directory_storage,'),
            ('transaction.reserve(UserAddressSpace, 1, .stable)', 'transaction.reserve(UserAddressSpace, 1, .replaceable)'),
            ('self.directory_storage.reclaimRetiredAfterBorrowExclusion()', 'skip_reclaim()'),
            ('const entry = &self.extra_slots[', 'self.get(index); const entry = &self.extra_slots['),
            ('"pushfq; pop %[flags]; cli"', '"pushfq; pop %[flags]"'),
            ('@atomicStore(u8, &self.directory_lock, 0, .release);', 'asm volatile ("sti"); @atomicStore(u8, &self.directory_lock, 0, .release);'),
            ('if (types.processPrincipalFromIndex(index) == null) return null;', ''),
            ('transaction.commitAdditionalStable()', 'transaction.commit()')):
        changed = dict(files)
        path = 'kernel/src/memory/address_space.zig'
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert as_directory_boundary(changed), old
    changed = dict(files)
    changed['kernel/src/memory/address_space.zig'] += '\nfn bad(self: anytype) void { _ = self.extra_slots; }\n'
    assert as_directory_boundary(changed)
    changed = dict(files)
    changed['kernel/src/unsafe_reclaim.zig'] = 'const bypass = owner.reclaimRetiredAfterBorrowExclusion;'
    assert as_directory_boundary(changed)
    # Each message-page transaction appends to the stable table owner; replacing
    # it would retire pages still reachable through previously published queues.
    for occurrence in (0, 1):
        changed = dict(files)
        path = 'kernel/src/state/ipc.zig'
        pieces = changed[path].split('messages.commitAdditionalStable()')
        assert len(pieces) == 3
        pieces[occurrence] += 'messages.commit()'
        changed[path] = 'messages.commitAdditionalStable()'.join(pieces[:occurrence + 1]) + 'messages.commitAdditionalStable()'.join(pieces[occurrence + 1:])
        assert as_directory_boundary(changed)
    for path, old, new in (
            ('kernel/src/memory/slot_table.zig', 'owner.transferRetiredAfterBorrowExclusion(domain)', 'true'),
            ('kernel/src/memory/slot_table.zig', '@atomicStore(?*Node, &self.root, null, .release);', '')):
        changed = dict(files)
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new, 1)
        assert as_directory_boundary(changed), old
    assert not context_scalar_boundary(files), context_scalar_boundary(files)
    for path, old, new in (
            ('kernel/src/scheduler_connection.zig', 'return setBootstrapThreadEntryForCpu(smp.currentCpuSlotIfKnown(), thread_index, owner, entry, stack);',
             'return setBootstrapThreadEntryForCpu(0, thread_index, owner, entry, stack);'),
            ('kernel/src/scheduler_connection.zig', 'kernel_runtime.kernel_state_ready or !bootTopologyRefreshAllowed(cpu,', 'false or !bootTopologyRefreshAllowed(cpu,'),
            ('kernel/src/scheduler_connection.zig', '!ctx.allocated or ctx.owner_process != owner or threadSlotReferencedByCpu(thread_index)', '!ctx.allocated'),
            ('kernel/src/scheduler_connection.zig', 'if (if_enabled) return false;', ''),
            ('kernel/src/scheduler_connection.zig', 'pinnedContextForCpu(cpu) orelse return false;', 'currentContextForXState() orelse return false;'),
            ('kernel/src/scheduler_connection.zig', 'out.* = .fromInt(ctx.fs_base);', 'out.* = .fromInt(ctx.frame.rip);'),
            ('kernel/src/boot/entry.zig', 'scheduler.currentUserEntrySnapshot(', 'unprotectedSnapshot('),
            ('kernel/src/traps.zig', 'scheduler.currentFsBaseSnapshot(', 'unprotectedSnapshot(')):
        changed = dict(files)
        assert old in changed[path], (path, old)
        changed[path] = changed[path].replace(old, new)
        assert context_scalar_boundary(changed), (path, old)
    for suffix in ('\nconst alias = scheduler.threadContext;', '\nconst alias = scheduler.threadXStateLoadable;'):
        changed = dict(files)
        changed['kernel/src/traps.zig'] += suffix
        assert context_scalar_boundary(changed)
    assert not cpu_pin_boundary(files), cpu_pin_boundary(files)
    for path, old, new in (
            ('kernel/src/scheduler_connection.zig', 'if (!verifiedAddThreadLocked(i, generation, initial_ready))',
             'if (!verifiedAddThread(i, generation, initial_ready))'),
            ('kernel/src/scheduler_connection.zig', 'if (!initializeThreadContextLocked(i, owner_process, user_cr3, initial_frame, initial_ready))',
             'if (!initializeThreadContextWithReadyState(i, owner_process, user_spaces, initial_frame, initial_ready))'),
            ('kernel/src/scheduler_connection.zig', 'state.current_context = ctx;', 'state.current_context = null; state.current_context = ctx;'),
            ('kernel/src/scheduler_connection.zig', 'requireLocalContextWriter(state);', ''),
            ('kernel/src/scheduler_connection.zig', '!ctx.allocated or ctx.generation != state.current_generation', '!ctx.allocated'),
            ('kernel/src/scheduler_connection.zig', 'state.current_context == ctx', 'state.enabled and state.current_thread == thread_index'),
            ('kernel/src/scheduler_connection.zig', 'if (cpu.current_context == ctx)', 'if (!cpu.is_idle and cpu.current_context == ctx)'),
            ('kernel/src/scheduler_connection.zig', 'clearCurrentContextLocked(state);', 'bypass_clear(state);'),
            ('kernel/src/scheduler_connection.zig', 'if (threadSlotReferencedByCpu(thread_index)) return false;', ''),
            ('kernel/src/smp.zig', '.indexOr(cpuCapacity());', '.indexOr(0);'),
            ('kernel/src/scheduler_connection.zig', 'for (scheduler_state.cpus)', 'for (scheduler_state.cpus[0..64])'),
            ('kernel/src/scheduler_connection.zig', 'for (scheduler_state.cpus, 0..)', 'for (scheduler_state.cpus[0..64], 0..)'),
            ('kernel/src/xstate_runtime.zig', 'scheduler.currentContextForXState()', 'scheduler.threadContextMutable(scheduler.currentThread())')):
        changed = dict(files)
        assert old in changed[path], (path, old)
        changed[path] = changed[path].replace(old, new)
        assert cpu_pin_boundary(changed), (path, old)
    changed = dict(files)
    changed['kernel/src/scheduler_connection.zig'] += '\nfn bypass() void { scheduler_state.cpus[0].current_context = null; }\n'
    assert cpu_pin_boundary(changed)
    changed = dict(files)
    changed['kernel/src/late_reset.zig'] = 'fn late() void { scheduler.initializeStaticStorage(); }'
    assert cpu_pin_boundary(changed)
    changed = dict(files)
    changed['kernel/src/scheduler_connection.zig'] = changed['kernel/src/scheduler_connection.zig'].replace(
        'fn deinit(self: TestCpuRows) void {\n        comptime std.debug.assert(builtin.is_test);',
        'fn deinit(self: TestCpuRows) void {', 1)
    assert cpu_pin_boundary(changed), 'dynamic topology destructor lost host-only admission'
    changed = dict(files)
    changed['kernel/src/syscalls.zig'] = changed['kernel/src/syscalls.zig'].replace('comptime std.debug.assert(@import("builtin").is_test);', '', 1)
    assert cpu_pin_boundary(changed), 'IRQ host fixture lost compile-time admission'

    assert not topology_boundary(files), topology_boundary(files)
    for path, old, new in (
            ('kernel/src/scheduler_connection.zig', 'smp.currentCpuSlotIfKnown(),', 'currentCpu(),'),
            ('kernel/src/scheduler_connection.zig', 'if (!bootTopologyRefreshAllowed(', 'if (bootTopologyRefreshAllowed('),
            ('kernel/src/scheduler_connection.zig', 'pub fn refreshTopology() void {',
             'pub fn refreshTopology() void { lockAllCpuSchedulerStates();'),
            ('kernel/src/scheduler_connection.zig', 'asm volatile ("cli; hlt")', 'asm volatile ("hlt")'),
            ('kernel/src/scheduler_connection.zig', '&scheduler_state.ap_user_dispatch_enabled, 1, .release',
             '&scheduler_state.ap_user_dispatch_enabled, 0, .release'),
            ('kernel/src/boot/entry.zig', 'scheduler.refreshTopology();', 'scheduler.enableApUserDispatch();'),
            ('bootloader/limine/kernel_entry.zig', 'entry.initializeLimineRuntimeOrHalt(smp_resources);',
             'entry.bootWithResources(resources, maybeResetAtCheckpoint);')):
        changed = dict(files)
        assert old in changed[path], (path, old)
        # Change every occurrence: a test/another halt must not consume the
        # only negative injection while leaving the production reset intact.
        changed[path] = changed[path].replace(old, new)
        assert topology_boundary(changed), (path, old)
    for name in ('refreshTopology', 'enableApUserDispatch', 'ap_user_dispatch_enabled'):
        changed = dict(files)
        changed['kernel/src/late_reset.zig'] = 'const alias = scheduler.' + name + ';'
        assert topology_boundary(changed), name
    assert not pmm_boundary(files), pmm_boundary(files)
    for old, new in (
            ('const std = @import("std");', 'const std = @import("../kernel.zig");'),
            ('self.head = null;', 'self.head = cursor;'),
            ('self.lock.unlock();', '')):
        changed = dict(files)
        path = 'kernel/src/memory/reclaim_cache.zig'
        assert old in changed[path]
        changed[path] = changed[path].replace(old, new)
        assert pmm_boundary(changed), ('reclaim cache', old)

    assert not vmo_storage_boundary(files), vmo_storage_boundary(files)
    for old, new in (
            ('const valid = self.matches(ref, ref.owner);', 'const valid = true;'),
            ('self.lock.unlock();\n            var draft = reservePath', 'var draft = reservePath'),
            ('self.lock.unlock();\n            if (has_parent)', 'if (has_parent)'),
            ('const allocator = p.allocator;', 'const allocator = other;'),
            ('const grant = p.grant;', 'const grant = other;'),
            ('pending = p;', 'pending = null;'),
            ('list = p.next;', 'list = null;'),
            ('self.root = null;', 'self.owner = 0;')):
        changed = dict(files)
        path = 'kernel/src/memory/vmo_backing.zig'
        assert old in changed[path], old
        changed[path] = changed[path].replace(old, new)
        assert vmo_storage_boundary(changed), (old,new)
    changed = dict(files)
    changed['kernel/src/state/vmo.zig'] += '\nfn unlocked() void { _ = types.vmo_backing_page_store; }\n'
    assert vmo_storage_boundary(changed)
    for old, new in (
            ('self.appendContiguousRange(pa, 1)', 'self.appendContiguousRange(pa, 2)'),
            ('quarantined(start, pages)', 'quarantined(start, 1)'),
            ('const std = @import("std");', 'const std = @import("../iommu.zig");')):
        changed = dict(files)
        changed['kernel/src/memory/free_page_list.zig'] = changed['kernel/src/memory/free_page_list.zig'].replace(old,new,1)
        assert pmm_boundary(changed), (old,new)
    changed = dict(files)
    changed['kernel/src/state/types.zig'] = changed['kernel/src/state/types.zig'].replace('.physicalRangeQuarantined);','.disableQuarantine);',1)
    assert pmm_boundary(changed)
    for bypass in ('const alias = module.WithQuarantine;',
                   'fn late() void { _ = module.WithQuarantine(alwaysFalse); }'):
        changed = dict(files)
        changed['bootloader/limine/late_allocator.zig'] = bypass
        assert pmm_boundary(changed)
    for path, old, new in (
            ('kernel/src/user_copy.zig', 'appendPage(candidate)', 'appendPage(0, candidate)'),
            ('kernel/src/user_copy.zig', 'appendPage(candidate)', 'appendPage(\n 0,\n candidate)'),
            ('kernel/src/user_copy.zig', 'appendPage(candidate)', 'seedContiguousRange(candidate, 1)'),
            ('kernel/src/memory/free_page_list.zig', 'if (endOf(descriptor(pa).?.*) > start) return error.FreeRangeOverlap;', 'if (false) return error.FreeRangeOverlap;')):
        changed = dict(files)
        assert old in changed[path], (path, old)
        changed[path] = changed[path].replace(old, new)
        assert pmm_boundary(changed), (path, old)
    for path, old, new in (
            ('kernel/src/memory/free_page_list.zig', 'try self.ownership.reserve(span);', '_ = span;'),
            ('kernel/src/memory/free_page_list.zig', 'errdefer self.ownership.move(span, .reserved, .allocated);', ''),
            ('kernel/src/memory/page_cache.zig', '.count = claimed', '.count = span_.count')):
        changed = dict(files)
        assert old in changed[path], (path, old)
        changed[path] = changed[path].replace(old, new)
        assert pmm_boundary(changed), (path, old)
    actual = census(files)
    if actual != expected:
        raise SystemExit('KernelState constructor callsites changed; audit fresh storage / fixture teardown: ' + str(actual))
    changed = dict(files)
    changed['kernel/src/new_live_reset.zig'] = 'fn bad(state: anytype) void { state.initPhase1InPlace(); }'
    assert census(changed) != expected
    print('kernel storage boundaries: actual migrations and bypass negative tests PASS')


if __name__ == '__main__':
    main()
