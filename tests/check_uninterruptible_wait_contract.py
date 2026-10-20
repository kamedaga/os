#!/usr/bin/env python3
"""uninterruptible wait ABI and actual caller boundaries; negative cases must fail this check."""
import re
from pathlib import Path
from check_kernel_storage_boundaries import function_body, production_source
ROOT = Path(__file__).resolve().parents[1]
SCHED = 'kernel/src/scheduler_connection.zig'
RUNTIME = 'userland/personality/linux/runtime/'

def c_function_body(text, name):
    match = re.search(r'\b'+re.escape(name)+r'\s*\([^;{]*\)\s*\{', text)
    if not match:
        raise ValueError('missing C function '+name)
    start = match.end(); depth = 1
    for end in range(start, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if not depth:
            return text[start:end]
    raise ValueError('unclosed C function '+name)

def source_files():
    paths = [SCHED, 'kernel/src/syscall/process.zig', 'kernel/src/syscall/runtime.zig',
             'kernel/src/syscall/fd.zig', 'kernel/src/syscall/native_ipc.zig',
             'kernel/abi/runtime_abi.zig', 'kernel/abi/fd_abi.zig', 'kernel/abi/ipc_abi.zig',
             'userland/libpacha/include/pacha/abi.h', 'musl/pachaos/include/pachaos/abi.h',
             'musl/pachaos/syscall/native_raw.h']
    paths += [str(p.relative_to(ROOT)) for p in (ROOT/RUNTIME).rglob('*.c')]
    paths += [RUNTIME+'support/native_wait.h']
    # All project-owned native callers, including test/bench and libc shims.
    for directory in ['userland','tests','musl/pachaos']:
        paths += [str(p.relative_to(ROOT)) for p in (ROOT/directory).rglob('*') if p.suffix in ['.c','.h']]
    return {p:(ROOT/p).read_text() for p in paths}

def check(files):
    errors=[]
    def require(ok,why):
        if not ok: errors.append(why)
    s=production_source(files[SCHED])
    require('signal_interrupt_consumed' not in s,'consumed exception remains')
    for name,flag in [('blockCurrentThreadPreferred','wait_uninterruptible'),('parkApThreadForBlockPreferred','wait_uninterruptible'),('tryWakeWaitTransferLocked','request.wait_uninterruptible')]:
        b=function_body(s,name)
        ctx='source_ctx' if name=='tryWakeWaitTransferLocked' else 'ctx'
        pattern=r'firstUnblockedPendingSignal\('+ctx+r'\) != 0 and '+ctx+r'\.signal_entry != 0 and\s*!'+re.escape(flag)
        require(bool(re.search(pattern,b)),name+' admission')
        # The rejecting branch may unlock, but the accepted path from saved
        # frame capture through blocked publication may not release T.
        start=b.find('var saved = frame.*;');end=b.find('verifiedBlockThread(current_thread);',start)
        if name=='tryWakeWaitTransferLocked':end=b.find('source.ownership = .blocked;',start)
        require(start>=0 and end>start and 'thread_table.unlock' not in b[start:end],name+' atomic block')
    for name in ['blockCurrentThread', 'parkApThreadForBlock']:
        b=function_body(s,name)
        require('return '+name+'Preferred(frame, wait_mailbox, wait_uninterruptible, wait_token, timeout_ticks, resume_rax, before_block, null);' in b, name+' forwards admission')
    b=function_body(s,'deliverSignalLocked')
    require('!ctx.wait_uninterruptible' in b,'signal U admission')
    for name in ['wakeIfTimerExpiredLocked','wakeIfWaitingTokenGenerationLocked','publishClaimedWaitLocked','cancelCurrentWaitToken','installExecContextForCurrentThread']:
        b=function_body(s,name)
        require('ctx.wait_uninterruptible = false;' in b,name+' retires attribute')
    for name in ['wakeIfTimerExpiredLocked','wakeIfWaitingTokenGenerationLocked']:
        require('ctx.wait_uninterruptible = wait_uninterruptible;' in function_body(s,name),name+' rollback attribute')
    require('wait_uninterruptible' not in files['kernel/src/syscall/process.zig'],'kill must ignore U')
    require('wait_uninterruptible' not in function_body(s,'releaseThreadLocked'),'release must ignore U')
    for p in ['kernel/abi/runtime_abi.zig','kernel/abi/fd_abi.zig','kernel/abi/ipc_abi.zig']:
        require('wait_uninterruptible: u64 = 1 << 1;' in files[p],p+' flag')
    for p in ['userland/libpacha/include/pacha/abi.h','musl/pachaos/include/pachaos/abi.h','musl/pachaos/syscall/native_raw.h']:
        require('WAIT_UNINTERRUPTIBLE (1ull << 1)' in files[p],p+' flag')
    for p,name,abi,access in [('kernel/src/syscall/runtime.zig','futexWait','runtime_abi','readUserU32'),('kernel/src/syscall/runtime.zig','futexWakeWaitWithOps','runtime_abi','futex_waiters_lock.lock'),('kernel/src/syscall/fd.zig','fdWaitMany','fd_abi','readPollItems'),('kernel/src/syscall/native_ipc.zig','recvWaitArgs','ipc_abi','readReceive(')]:
        b=function_body(files[p],name)
        require(0<=b.find('flags & ~'+abi+'.wait_uninterruptible')<b.find(access),name+' flags before effects')
        if name == 'recvWaitArgs':
            require('switchIpc(h, frame, flags, wait_token, block_ticks, preferred, null)' in b, name+' flag forwarding')
            b = function_body(files[p], 'switchIpc')
        require('(flags & '+abi+'.wait_uninterruptible) != 0' in b,name+' flag forwarding')
    b=function_body(files['kernel/src/syscall/native_ipc.zig'],'notifyWait')
    require(0<=b.find('flags & ~ipc_abi.notify_wait_known_flags')<b.find('notifyPeer('),'notify flags before outbound')
    # Twelve internal classes share these seven futex sites and RPC/session wrappers.
    internal=['lpr_common/runtime_support.c','lpr_fd/control.c','lpr_block/client.c','lpr_socket.c','lpr_vfs/session.c','lpr_process/syscalls.c']
    for rel in internal:
        t=files[RUNTIME+rel]
        for m in re.finditer(r'lpr_pacha_syscall4\(\s*PACHA\w*FUTEX_WAIT,',t):
            tail=t[m.end():t.find(';',m.end())]
            require('PACHA_WAIT_UNINTERRUPTIBLE' in tail,rel+' internal futex U')
    t=files[RUNTIME+'support/native_wait.h']
    for name in ['lpr_native_ipc_recv_wait', 'lpr_native_fd_wait_writable', 'lpr_native_ipc_call_wait']:
        body=c_function_body(t,name)
        require('PACHA_WAIT_UNINTERRUPTIBLE' in body,name+' internal RPC U')
    body=c_function_body(t,'lpr_native_ipc_call_wait')
    require('lpr_ipc_message' in body and 'if (sent ||' in body,
            'committed synchronous call cannot replay')
    for p in [RUNTIME+'lpr_wait.c',RUNTIME+'lpr_dispatch.c',RUNTIME+'lpr_vfs/ops.c']:
        require('WAIT_UNINTERRUPTIBLE' not in files[p],p+' outward U')
    require('graph->native_not_ready = 1;' in files[RUNTIME+'lpr_wait.c'],'graph NOT_READY output')
    require('*wait_not_ready |= graph->native_not_ready;' in files[RUNTIME+'lpr_wait.c'],'graph output after cleanup')
    for p,t in files.items():
        if p.endswith((".c", ".h")):
            require(not re.search(r'(?:lpr_pacha_syscall3|pacha_syscall3|__pachaos_raw3)\(\s*PACHA\w*FUTEX_WAIT',t),p+' old futex arity')
    return errors

def main():
    files=source_files();errors=check(files)
    assert not errors,errors
    mutations=[]
    for name in ['blockCurrentThreadPreferred','parkApThreadForBlockPreferred','tryWakeWaitTransferLocked']:
        text=files[SCHED];body=function_body(text,name)
        old='!request.wait_uninterruptible' if name.startswith('try') else '!wait_uninterruptible'
        mutations.append((name,SCHED,body,body.replace(old,'false',1)))
    b=function_body(files[SCHED],'blockCurrentThreadPreferred')
    mutations.append(('T released between check and block',SCHED,b,b.replace('var saved = frame.*;','var saved = frame.*;\n scheduler_state.thread_table.unlock();\n scheduler_state.thread_table.lock();',1)))
    body=c_function_body(files[RUNTIME+'support/native_wait.h'],'lpr_native_ipc_call_wait')
    mutations += [
        ('synchronous call missing U',RUNTIME+'support/native_wait.h',body,body.replace('PACHA_WAIT_UNINTERRUPTIBLE','0')),
        ('committed call replay',RUNTIME+'support/native_wait.h',body,body.replace('if (sent ||','if (')),

        ('signal ignores U',SCHED,'and !ctx.wait_uninterruptible and','and true and'),
        ('retirement omission',SCHED,function_body(files[SCHED],'cancelCurrentWaitToken'),function_body(files[SCHED],'cancelCurrentWaitToken').replace('ctx.wait_uninterruptible = false;','')),
        ('internal missing U',RUNTIME+'lpr_block/client.c','PACHA_WAIT_UNINTERRUPTIBLE','0'),
        ('outward wrongly U',RUNTIME+'lpr_wait.c','PACHA_FD_WAIT_FOREVER,\n                    0);','PACHA_FD_WAIT_FOREVER,\n                    PACHA_WAIT_UNINTERRUPTIBLE);'),
        ('kill waits for U',SCHED,function_body(files[SCHED],'releaseThreadLocked'),function_body(files[SCHED],'releaseThreadLocked').replace('if (!ctx.allocated)', 'if (ctx.wait_uninterruptible) return false;\n    if (!ctx.allocated)',1)),
    ]
    for label,path,old,new in mutations:
        assert old in files[path],label
        changed=dict(files);changed[path]=changed[path].replace(old,new,1)
        assert check(changed),label
        print('uninterruptible wait injection PASS:',label)
    print('uninterruptible wait ABI/caller/lifetime boundaries PASS')
if __name__=='__main__':main()
