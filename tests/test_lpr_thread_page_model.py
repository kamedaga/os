# SPDX-License-Identifier: MIT
from collections import deque
# Ordered ASM loads: enabled/cpu1/seq1/cpu2/current/seq2/enabled.
# Remote writers split odd/current/even stores; a target CPU's writer must
# finish before reader execution resumes there. At most two migrations and
# two foreign writers suffice to include A->B->A and same-CPU ABA witnesses.
def explore(broken=''):
 # cpu,seq0,tid0,seq1,tid1,pending0,pending1,step,pointer,s1,value,migrations,enabled,same_cpu_aba
 initial=(0,2,11,2,22,0,0,0,-1,0,0,0,1,0)
 q=deque([(initial,())]);seen={initial};accepted=0;witness=None
 while q:
  s,path=q.popleft();cpu,a,ta,b,tb,p0,p1,step,ptr,seq,val,mig,enabled,aba=s
  ss=[a,b];tt=[ta,tb];pending=[p0,p1]
  if step==7:
   accepted+=1
   if val!=11 or not enabled:return len(seen),accepted,path
   continue
  def push(v,event):
   v=tuple(v)
   if v not in seen:seen.add(v);q.append((v,path+(event,)))
  v=list(s)
  if step==0:
   if enabled:v[7]=1;push(v,'enabled1')
  elif step==1:v[8]=cpu;v[7]=2;push(v,'cpu1='+str(cpu))
  elif step==2:
   if ss[ptr] and ss[ptr]%2==0:v[9]=ss[ptr];v[7]=3;push(v,'seq1='+str(ss[ptr]))
  elif step==3:
   if broken=='no-cpu' or ptr==cpu:v[7]=4;push(v,'cpu2='+str(cpu))
  elif step==4:v[10]=tt[ptr];v[7]=5;push(v,'current='+str(tt[ptr]))
  elif step==5:
   if ss[ptr]==seq:v[7]=6;push(v,'seq2='+str(ss[ptr]))
  elif step==6:
   if enabled or broken=='no-final-enabled':v[7]=7;push(v,'enabled2')
  # Migration starts a foreign writer on the departed CPU. The arriving CPU
  # publishes caller identity completely before its first user instruction.
  if mig<2 and not pending[1-cpu]:
   v=list(s);target=1-cpu;v[0]=target;v[11]+=1
   if broken!='no-return-publish':
    v[1+target*2]+=2;v[2+target*2]=11
   v[5+cpu]=1;push(v,'migrate '+str(cpu)+'->'+str(target))
  # Reader is fully descheduled while a same-process sibling runs on this
  # exact CPU. No reader load executes in the interval. Its next user return
  # must publish caller A again even though TSC_AUX never changed.
  if not aba and not pending[cpu]:
   v=list(s);v[13]=1;v[1+cpu*2]+=2;v[2+cpu*2]=22
   if broken!='no-same-return-publish':v[1+cpu*2]+=2;v[2+cpu*2]=11
   push(v,'same-CPU sibling B -> return A')
  other=1-cpu
  if pending[other]:
   v=list(s);p=pending[other]
   if p==1:
    if broken!='no-odd':v[1+other*2]+=1
    v[5+other]=2
   elif p==2:v[2+other*2]=22;v[5+other]=3
   else:
    v[1+other*2]+=2 if broken=='no-odd' else 1;v[5+other]=0
   push(v,'writer cpu'+str(other)+' store'+str(p))
  if enabled:
   v=list(s);v[12]=0;push(v,'close')
 return len(seen),accepted,None
n,a,w=explore();assert a and w is None,(n,a,w)
print('READER_MODEL safe states=%d accepted=%d mismatch=0'%(n,a))
for variant in ['no-odd','no-cpu','no-return-publish','no-same-return-publish','no-final-enabled']:
 n,a,w=explore(variant)
 assert w is not None,(variant,n,a)
 print('BROKEN_DETECTED',variant,' -> '.join(w))
