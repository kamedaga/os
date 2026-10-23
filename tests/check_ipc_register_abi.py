#!/usr/bin/env python3
"""Reject old native IPC boundaries and audit the actual C register wrapper."""
from pathlib import Path
import re, subprocess
ROOT=Path(__file__).resolve().parents[1]
OPS=r'(?:SEND|RECV|CALL|REPLY|REPLY_RECV|RECV_WAIT)\b'
OLD=re.compile(r'\b(?:pacha_syscall[0-6](?:_committed)?|lpr_pacha_syscall[0-6](?:_committed)?|__pachaos_raw[0-6])\s*\(\s*(?:PACHA_IPC_SYSCALL_'+OPS+r'|PACHAOS_SYSCALL_IPC_'+OPS+r'|(?:78|79|80|81|82|84)\s*,)')
def main():
    violations=[]
    for directory in ['userland','musl/pachaos','tests']:
        for p in (ROOT/directory).rglob('*'):
            if p.suffix not in ['.c','.h','.S','.zig']: continue
            if OLD.search(p.read_text()): violations.append(str(p.relative_to(ROOT)))
    assert not violations, violations
    abi=(ROOT/'kernel/abi/ipc_abi.zig').read_text()
    assert not re.search(r'\bmsg_(?:size|word\d_offset|fd_\w+_offset|flags_offset)\b',abi)
    for name,nr in [('send',78),('recv',79),('call',80),('reply',81),('reply_recv',82),('recv_wait',84)]:
        assert f'syscall_ipc_{name}: u64 = {nr};' in abi
    # Negative controls: all six operations and both native symbol families.
    for op in ['SEND','RECV','CALL','REPLY','REPLY_RECV','RECV_WAIT']:
        assert OLD.search(f'pacha_syscall2(PACHA_IPC_SYSCALL_{op}, 1, 2)')
        assert OLD.search(f'__pachaos_raw5(PACHAOS_SYSCALL_IPC_{op}, 1, 2)')
    assert not OLD.search('pacha_syscall0(PACHA_IPC_SYSCALL_REPLY_EXTRACT)')
    out=ROOT/'.artifacts/tests/ipc-register-abi';out.mkdir(parents=True,exist_ok=True)
    subprocess.run(['clang','-O2','-ffreestanding','-fno-stack-protector','-I'+str(ROOT/'userland/libpacha/include'),
        '-c',str(ROOT/'userland/libpacha/src/syscall.c'),'-o',str(out/'syscall.o')],check=True)
    asm=subprocess.check_output(['objdump','-d','--disassemble=pacha_ipc_registers',str(out/'syscall.o')],text=True)
    (out/'wrapper.asm').write_text(asm)
    assert asm.count('\tsyscall') == 1
    for reg in ['rbx','r12','r13','r15']:
        assert re.search(r'push\s+%'+reg,asm) and re.search(r'pop\s+%'+reg,asm), reg
    for reg in ['r10','r8','r9','r12','r13','rbx','r15']:
        assert '%'+reg in asm
    print('IPC register ABI: old callsites/header absent; current numbers; 12 negative controls; RBX/R12/R13/R15 preservation PASS')
if __name__=='__main__': main()
