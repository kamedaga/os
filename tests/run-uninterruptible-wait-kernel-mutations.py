#!/usr/bin/env python3
"""Compile and execute actual scheduler/frontend tests against isolated faults."""
from pathlib import Path
import sys, shutil, subprocess, json
from check_kernel_storage_boundaries import function_body
root=Path(__file__).resolve().parents[1]
out=root/'.artifacts/kernel-refactor-stage1'
work=out/'uninterruptible-wait-mutations'
work.mkdir(exist_ok=True)
for name in ['src','abi']:
 shutil.copytree(root/'kernel'/name,work/'kernel'/name,dirs_exist_ok=True)
obj=next(root.glob('kernel/.zig-cache/o/*/pacha_eevdf_context_test.o'))
p=work/'kernel/src/scheduler_connection.zig';original=p.read_text()
cmd=['zig','test','-fllvm',str(obj),'-ODebug','--dep','kernel_abi_root','-Mroot='+str(work/'kernel/src/uninterruptible_wait_test.zig'),'-Mkernel_abi_root='+str(work/'kernel/abi/kernel_abi_root.zig'),'--test-filter','uninterruptible wait']
cases=[]
for fn in ['blockCurrentThreadPreferred','parkApThreadForBlockPreferred','tryWakeWaitTransferLocked']:
 b=function_body(original,fn);needle='!request.wait_uninterruptible' if fn.startswith('try') else '!wait_uninterruptible'
 cases.append((fn,b,b.replace(needle,'false',1)))
b=function_body(original,'blockCurrentThreadPreferred')
cases.append(('T-gap',b,b.replace('var saved = frame.*;','var saved = frame.*;\n scheduler_state.thread_table.unlock();\n scheduler_state.thread_table.lock();',1)))
b=function_body(original,'deliverSignalLocked');cases.append(('signal-U',b,b.replace('and !ctx.wait_uninterruptible and','and true and',1)))
b=function_body(original,'wakeIfTimerExpiredLocked');cases.append(('retire-U',b,b.replace('ctx.wait_uninterruptible = false;','',1)))
b=function_body(original,'releaseThreadLocked');cases.append(('kill-U',b,b.replace('if (!ctx.allocated)','if (ctx.wait_uninterruptible) return false;\n    if (!ctx.allocated)',1)))
results=[]
try:
 for label,old,new in [('control',original,original)]+cases:
  assert old in original
  p.write_text(original.replace(old,new,1))
  log=out/('uninterruptible-wait-mutation-'+label+'.log')
  with log.open('w') as f:r=subprocess.run(cmd,cwd=work/'kernel',stdout=f,stderr=subprocess.STDOUT,timeout=90)
  text=log.read_text()
  tested='scheduler_connection.test.' in text
  ok=tested and (r.returncode==0 if label=='control' else r.returncode!=0)
  results.append(dict(case=label,exit=r.returncode,real_tests_ran=tested,expected_outcome=ok,log=log.name))
  print(label,'PASS' if ok else 'FAIL',flush=True)
  if not ok:break
finally:p.write_text(original)
(out/'uninterruptible-wait-kernel-mutations.json').write_text(json.dumps(results,indent=2)+'\n')
assert len(results)==8 and all(x['expected_outcome'] for x in results),results
