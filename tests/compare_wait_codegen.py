#!/usr/bin/env python3
"""Instruction counts for real ELF bodies, including DWARF inline ranges."""
import re,subprocess,json,sys
from pathlib import Path
NAMES=['traps.syscallEntryCommon','traps.syscallUserReturnCr3FromAsm','scheduler_connection.switchToGenerationLockedWithRegisters','scheduler_connection.handoffToReadyThreadGenerationWithRax','scheduler_connection.blockCurrentThread','scheduler_connection.parkApThreadForBlock','scheduler_connection.tryWakeWaitTransferLocked','scheduler_connection.deliverSignalLocked']
TYPES=['scheduler_connection.ThreadContext','scheduler_connection.ThreadHotState','scheduler_connection.WakeWaitTransfer','interrupts.TrapFrame']
def capture(path):
 info=subprocess.check_output(['readelf','--debug-dump=info',path],text=True)
 dies={};cur=None
 for line in info.splitlines():
  m=re.match(r'\s*<\d+><([0-9a-f]+)>:.*?\((DW_TAG_\w+)\)',line)
  if m:cur={'tag':m[2]};dies[int(m[1],16)]=cur;continue
  if re.match(r'\s*<\d+><[0-9a-f]+>:',line):cur=None
  if cur is not None:
   m=re.search(r'DW_AT_(\w+)\s*:\s*(.*)',line)
   if m:cur[m[1]]=m[2]
 ranges={}
 for line in subprocess.check_output(['readelf','--debug-dump=Ranges',path],text=True).splitlines():
  m=re.match(r'\s*([0-9a-f]+)\s+([0-9a-f]{16})\s+([0-9a-f]{16})\s*$',line)
  if m:ranges.setdefault(int(m[1],16),[]).append((int(m[2],16),int(m[3],16)))
 instructions={}
 for line in subprocess.check_output(['objdump','-d','--no-show-raw-insn',path],text=True).splitlines():
  m=re.match(r'\s*([0-9a-f]+):\s+(.+)',line)
  if m:instructions[int(m[1],16)]=m[2]
 def text(x):return x.split('): ')[-1]
 def spans(d):
  if 'ranges' in d:return ranges[int(d['ranges'],16)]
  if 'low_pc' in d:
   lo=int(d['low_pc'],16);hi=int(d['high_pc'],16)
   return [(lo,hi if hi>=lo else lo+hi)]
  return []
 result={};sets={}
 for name in NAMES:
  bases=[(off,d) for off,d in dies.items() if re.sub(r'__anon_\d+','',text(d.get('linkage_name','')))==name]
  assert len(bases)==1,(name,len(bases))
  off,d=bases[0];ss=spans(d)
  if not ss:
   for inst in dies.values():
    if inst['tag']=='DW_TAG_inlined_subroutine' and inst.get('abstract_origin')==f'<0x{off:x}>':ss+=spans(inst)
  addrs={a for a in instructions if any(lo<=a<hi for lo,hi in ss)}
  assert addrs,name
  sets[name]=addrs
  result[name]={'instructions':len(addrs),'calls':sum(instructions[a].startswith('call') for a in addrs),'inlined':'inline' in d,'range_count':len(ss)}
 bsp=sets[NAMES[4]]-sets[NAMES[5]]
 result['BSP block excluding AP inline ranges']={'instructions':len(bsp),'calls':sum(instructions[a].startswith('call') for a in bsp),'inlined':False}
 sizes={}
 for typ in TYPES:
  matching=[d for d in dies.values() if d['tag']=='DW_TAG_structure_type' and text(d.get('name',''))==typ and 'byte_size' in d]
  if matching:sizes[typ]=int(matching[0]['byte_size'])
 return {'bodies':result,'sizes':sizes}
a,b,out=sys.argv[1:];old=capture(a);new=capture(b)
rows=[dict(name=n,before=v,after=new['bodies'][n],delta=new['bodies'][n]['instructions']-v['instructions']) for n,v in old['bodies'].items()]
result=dict(before=a,after=b,bodies=rows,sizes_before=old['sizes'],sizes_after=new['sizes'],method='ReleaseSmall real ELFs; objdump instructions in DWARF ranges. Inline ranges include nested callees; counts are static, not latency/cycle measurements. Syscall entry/return assembly is in syscallEntryCommon; return staging helper separately counted.')
Path(out).write_text(json.dumps(result,indent=2)+'\n')
for r in rows:print(r['name'],r['before']['instructions'],r['after']['instructions'],f"{r['delta']:+}")
print('sizes',old['sizes'],new['sizes'])
