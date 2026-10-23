"""Exhaustive single-remote-CPU TSO protocol model, not a timing litmus.

Stores may drain separately; MFENCE drains the owner's store buffer. Locked
CAS drains the requester buffer and is a single coherent row RMW. Each trace
covers enter, two serialized requests, IRQ flush/ACK without row restoration,
user TLB refill, and an optional user->kernel transition. A local flushing CR3
load is serializing; IF=0 defers IRQ service until the user boundary.
"""
import dataclasses as d
import unittest

@d.dataclass(frozen=True)
class S:
    e: int = 0
    r: int = 0
    request: int = 1
    row: int = 1
    active: bool = False
    active_buffer: bool = False
    pte: int = 0
    pte_buffer: int = 0
    mismatch: bool = False
    flush: bool = False
    user: bool = False
    tlb: int = 0
    target: bool = False
    ack: bool = False
    observed: int = 0
    raced: bool = False
    cas_between_load_store: bool = False
    exited: bool = False


def successors(s, mutant=None):
    def step(event, **changes):
        return event, d.replace(s, **changes)
    # Explicit TSO propagation, including publication before its MFENCE.
    if s.active_buffer:
        yield step('drain active', active=True, active_buffer=False)
    if s.pte_buffer:
        yield step('drain PTE', pte=s.pte_buffer, pte_buffer=0)
    if s.e == 0:
        yield step('publish active buffered', e=1, active_buffer=True)
    elif s.e == 1:
        if mutant == 'no_enter_fence':
            yield step('missing enter MFENCE', e=2)
        else:
            yield step('enter MFENCE', e=2, active=True, active_buffer=False)
    elif s.e == 2:
        yield step('load row', e=3 if s.row != 1 else 5,
                   mismatch=s.row != 1, flush=s.row != 1)
    elif s.e == 3:
        yield step('mismatch store row', e=4, row=1,
                   raced=s.cas_between_load_store)
    elif s.e == 4:
        yield step('mismatch MFENCE', e=5, active=True, active_buffer=False)
    elif s.e == 5:
        yield step('CR3 load + IF=1', e=6, user=True,
                   tlb=s.pte if s.flush else s.tlb)
    if s.r == 0 and s.request <= 2:
        yield step('PTE update buffered', r=1, pte_buffer=s.request)
    elif s.r == 1:
        yield step('request MFENCE', r=2, pte=s.request, pte_buffer=0)
    elif s.r == 2:
        yield step('locked CAS row->0', r=3, row=0 if s.row == 1 else s.row,
                   cas_between_load_store=s.cas_between_load_store or s.e == 3)
    elif s.r == 3:
        target = s.active
        if mutant == 'row_targets':
            target = s.row == 1
        yield step('snapshot ALL-online active', r=4, target=target, ack=False)
    elif s.r == 4:
        if not s.target or s.ack:
            yield step('request complete', r=0, request=s.request+1,
                       observed=s.request)
    # Interrupts cannot ACK inside the IF-clear entry transaction. The
    # abstract receiver leaves active set and row untouched, a conservative
    # behavior which exercises repeated requests within one active period.
    if s.r == 4 and s.target and not s.ack and s.user:
        yield step('IPI flush + ACK', ack=True, tlb=s.pte)
    if s.user:
        if s.tlb == -1:
            yield step('user refill', tlb=s.pte)
        # Optional exit proves active clear only after leaving user mappings.
        if not s.exited and s.request > 2:
            yield step('kernel CR3 then leave', active=False, user=False, exited=True)


def explore(initial, mutant=None):
    todo=[initial]; seen={initial: None}; witnesses={}
    while todo:
        s=todo.pop()
        # Once the request has completed, a user access must not use a
        # translation older than it; an invalid TLB will refill from PTEs.
        if s.user and s.tlb >= 0 and s.tlb < s.observed:
            trace=[];cur=s
            while seen[cur]:
                prev,event=seen[cur];trace.append(event);cur=prev
            return seen, list(reversed(trace)), witnesses
        if s.raced:witnesses['mismatch_store_after_CAS']=True
        if s.request == 3 and s.user:witnesses['two_requests']=True
        for event,n in successors(s,mutant):
            if n not in seen:seen[n]=(s,event);todo.append(n)
    return seen,None,witnesses


class Protocol(unittest.TestCase):
    def test_all_interleavings(self):
        total=0;witnesses={}
        # Both retained and mismatching loads, including an invalid TLB and
        # old retained translations, and an already-running row-zero CPU.
        for initial in [S(row=r,tlb=t) for r in (0,1,2) for t in (-1,0)]+[
                S(e=6,row=0,active=True,user=True),
                S(e=6,row=1,active=True,user=True)]:
            states,bad,w=explore(initial);total+=len(states);witnesses.update(w)
            self.assertIsNone(bad,bad)
        self.assertTrue(witnesses.get('mismatch_store_after_CAS'))
        self.assertTrue(witnesses.get('two_requests'))
        print('TLB_MODEL states=%d witnesses=%s' % (total, witnesses))

    def test_negative_row_based_targets(self):
        _,bad,_=explore(S(e=6,row=0,active=True,user=True),'row_targets')
        self.assertIsNotNone(bad)

    def test_negative_missing_enter_fence(self):
        _,bad,_=explore(S(),'no_enter_fence')
        self.assertIsNotNone(bad)

if __name__ == '__main__':unittest.main()
