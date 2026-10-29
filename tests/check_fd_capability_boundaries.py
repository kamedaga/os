#!/usr/bin/env python3
"""Canonical entry authority / writer inventory tripwires, not a concurrency proof."""
from pathlib import Path
import hashlib,re
from check_kernel_address_boundaries import function_body, code_only, without_tests
ROOT=Path(__file__).resolve().parents[1]
WRITERS={
 'state/fd.zig':['createSerialFdAt','installFdPrepared','closeFd','closeFdWithFreeList','closeCloexecFdsWithFreeList','replaceFd','setFdFlags','transferFd','revokeObjectHolders'],
 'state/process.zig':['grantFdsForProcessCreate','cloneFdTableForFork'],
 'state/vma.zig':['releaseFdTableForProcessIndex','releaseFdTableForProcessIndexWithFreeList'],
}
def check(files):
 errors=[]
 def body(path,name):return function_body(code_only(files[path]),name)
 for path,names in WRITERS.items():
  for name in names:
   b=body(path,name)
   if '.write(' not in b and 'entry.replace(' not in b:errors.append(name+': missing per-entry writer')
   if 'entry.* = ' in b or ').* = ' in b:errors.append(name+': bypass atomic authority write')
 t='state/types.zig';s=code_only(files[t]);entry=s[s.index('pub const FdEntry'):s.index('pub const FdInfo')]
 replace=function_body(entry,'replace')
 for token in ('&self.seq, odd, .seq_cst','&self.object.kind, value.object.kind, .seq_cst','&self.object.index, value.object.index, .seq_cst','&self.object.generation, value.object.generation, .seq_cst','@bitCast(value.rights), .seq_cst','@bitCast(value.flags), .seq_cst','if (odd == std.math.maxInt(u64)) odd else odd + 1'):
  if token not in replace:errors.append('atomic writer: '+token)
 read=function_body(entry,'read')
 if 'seq & 1 != 0' not in read or 'self.current(seq)' not in read or not read.index('&self.seq, .seq_cst')<read.index('self.readFields(seq)')<read.index('self.current(seq)'):errors.append('seq double check')
 fields=function_body(entry,'readFields')
 if fields.count('@atomicLoad(')!=5 or fields.count('.seq_cst')!=5:errors.append('torn field loads')
 calls={path:without_tests(code_only(text)).count('.pinFdCapability(') for path,text in files.items() if '.pinFdCapability(' in text}
 calls={path:count for path,count in calls.items() if count}
 if calls!={}:errors.append('unreviewed FD loan caller: '+str(calls))
 if 'writeMsr(msr_ia32_fmask, rflags_if | rflags_df)' not in files['arch/x86_64/platform.zig']:errors.append('syscall IRQ masking')
 c='state/fd_capability.zig'
 retire=body(c,'retireTable')
 if not retire.index('self.object_directory_leaf.lock()')<retire.index('table.entries.retire()')<retire.index('self.object_directory_leaf.unlock(flags)'):errors.append('table withdrawal leaf')
 grow=body('state/process.zig','ensureProcessCapacity')
 if 'self.fd_tables_extra.allocate(' not in grow or 'while (self.fd_tables_extra.live < extra_count)' not in grow:errors.append('incremental FD process directory')
 if 'new_fd' in grow or 'fd_grants' in grow:errors.append('whole FD directory copy')
 table=s[s.index('pub const FdTable'):s.index('pub const FdTransferMode')]
 write=function_body(table,'write')
 if 'self.at(i).replace(value)' not in write or 'self.entries.mark(' not in write:errors.append('entry writer/availability index bypass')
 growth=function_body(table,'ensureCapacity')
 if 'self.entries.allocate(allocator, leaf,' not in growth or 'while (self.entries.live < target)' not in growth:errors.append('incremental entry growth')
 if '@memcpy' in table or 'Draft' in table:errors.append('table copying')
 if 'firstMarkedAtOrAfter' not in body('state/fd.zig','findFreeFd'):errors.append('linear free FD search')
 slot='memory/slot_table.zig'
 if 'owner.transferRetiredAfterBorrowExclusion(domain)' not in body(slot,'reclaim'):errors.append('rejected grant ownership transfer')
 withdraw=body(slot,'withdraw')
 if 'entry.withdrawn = true' not in withdraw or 'free_head' in withdraw:errors.append('slot quarantine')
 fast=body('syscall/native_ipc.zig','fastDispatchAfterRegisterAdmission')
 # Freeze the K borrow interval: callbacks may abandon the stack or drop K.
 if hashlib.sha256(re.sub(r'\s+','',fast).encode()).hexdigest() != '6160b1d085898ed7fcc3a230a669103f833c77091d9681424beac954b8569848':errors.append('K IPC borrower changed: audit last pointer use before callbacks')
 admission=body('syscalls.zig','syscallIpcScalarDispatch')
 if 'fastDispatchKFree' in admission or 'kernel_state_lock.lock(' not in admission or not admission.index('kernel_state_lock.lock(')<admission.index('fastDispatchAfterRegisterAdmission('):errors.append('IPC fallback must own K')
 borrow=body(c,'resolveIpcUnderK')
 if 'resolveIpcUnderK(false,' not in body(c,'directIpcUnderK') or 'resolveIpcUnderK(true,' not in body(c,'ipcUnderK'):errors.append('shared direct/full authority validation bypass')
 for token in ('types.isFdRightsSubset(rights, entry.rights)', 'entry.authority.ipc_generation != body.generation', '!body.active', '!body.live.isOpen()'):
  if token not in borrow:errors.append('K borrow authority: '+token)
 if '.enter(' in borrow or 'getBorrowed(' in borrow or 'resolveBorrowed(' in borrow:errors.append('runtime borrowed reader restored')
 for path,text in files.items():
  source=without_tests(code_only(text))
  if re.search(r'fd_qsbr|pinFdCapability|resolveBorrowed|getBorrowed|fastDispatchKFree|releaseRetainedReply',source):errors.append('removed FD reader mechanism: '+path)
 owner=body('memory/owned_storage.zig','transferRetiredAfterBorrowExclusion')
 for token in ('try self.bind()', 'self.retired = record.next', 'self.pending = record.next', 'queue.push(record)', 'queue.reclaim()'):
  if token not in owner:errors.append('detached owner: '+token)
 queue=code_only(files['memory/owned_storage.zig']).split('pub const RetiredQueue = struct {')[1].split('pub fn requiredPages')[0]
 returned=function_body(queue,'reclaim')
 for token in ('self.head = null', 'self.lock.unlock()', 'allocator.returnRecord(record, extent)', 'else self.push(record)'):
  if token not in returned:errors.append('queue return ownership: '+token)
 if 'self.lock.unlock()' in returned and 'allocator.returnRecord' in returned and returned.index('self.lock.unlock()')>returned.index('allocator.returnRecord'):errors.append('allocator under queue leaf')
 registry=body('memory/reclaim_cache.zig','reclaim')
 if 'domain.reclaim()' not in registry or any(x in registry for x in ('kernel_state_lock','reclaimFdCapabilities','.retire(')):errors.append('PMM must reach detached grants without K')
 for name,table in [('createIpcEndpoint','ipc_endpoints'),('createIpcChannel','ipc_channels')]:
  create=body('state/ipc.zig',name)
  if 'messages.rollback()' not in create or f'self.{table}.reclaim()' not in create or f'self.{table}.return_queue = &self.fd_retired' not in create:errors.append('message rollback PMM reachability: '+name)
 allocate=body('kernel.zig','allocatePool')
 if 'defer if (comptime' not in allocate or 'table.reclaim()' not in allocate:errors.append('successful allocation draft rollback must reach PMM')
 return errors
if __name__=='__main__':
 files={str(p.relative_to(ROOT/'kernel/src')):p.read_text() for p in (ROOT/'kernel/src').rglob('*.zig')}
 assert not check(files),check(files)
 assert not (ROOT/'kernel/src/memory/immutable_snapshot.zig').exists()
 count=0
 for path,names in WRITERS.items():
  for name in names:
   changed=dict(files);b=function_body(files[path],name)
   changed[path]=files[path].replace(b,b.replace('.write(','.bypass(').replace('entry.replace(','entry.bypass('))
   assert check(changed),name
   count+=1
 for path,old,new in (
  ('syscall/native_ipc.zig','const borrowed =','kernel_state_lock.unlock(); const borrowed ='),
  ('syscalls.zig','kernel_state_lock.lock(if (lock_profile.enabled) lock_profile.classify(nr) else {});','missingK();'),
  ('state/fd_capability.zig','types.isFdRightsSubset(rights, entry.rights)','true'),
  ('state/fd_capability.zig','entry.authority.ipc_generation != body.generation','false'),
  ('state/types.zig','self.at(i).replace(value)','bypass(value)'),
  ('state/types.zig','&self.seq, odd, .seq_cst','&self.seq, odd, .monotonic'),
  ('state/types.zig','&self.object.index, value.object.index, .seq_cst','&self.object.index, value.object.index, .monotonic'),
  ('state/types.zig','if (self.current(seq))','if (true)'),
  ('state/types.zig','seq & 1 != 0','false'),
  ('state/types.zig','&self.object.generation, .seq_cst','&self.object.generation, .monotonic'),
  ('state/fd_capability.zig','body.live.isOpen()','true'),
  ('memory/slot_table.zig','owner.transferRetiredAfterBorrowExclusion(domain)','owner.reclaimRetiredAfterBorrowExclusion()'),
  ('arch/x86_64/platform.zig','writeMsr(msr_ia32_fmask, rflags_if | rflags_df)','writeMsr(msr_ia32_fmask, rflags_df)'),
  ('memory/owned_storage.zig','queue.push(record)','dropRecord(record)'),
  ('memory/owned_storage.zig','self.pending = record.next','self.pending = null'),
  ('state/ipc.zig','self.ipc_endpoints.reclaim()','skipEndpointRollback()'),
  ('state/ipc.zig','self.ipc_channels.reclaim()','skipChannelRollback()'),
  ('kernel.zig','table.reclaim()','skipDraftRollback()'),
  ('memory/owned_storage.zig','else self.push(record)','else {}'),
  ('memory/reclaim_cache.zig','domain.reclaim()','0'),
  ('state/process.zig','self.fd_tables_extra.allocate(','copyWholeDirectory('),
  ('state/fd.zig','table.entries.firstMarkedAtOrAfter(','linearScan('),
 ):
  assert old in files[path],old
  changed=dict(files);changed[path]=files[path].replace(old,new)
  assert check(changed),old
  count+=1
 print(f'FD canonical entry boundaries: PASS ({count} mutation controls, {sum(map(len,WRITERS.values()))} writer bodies)')
