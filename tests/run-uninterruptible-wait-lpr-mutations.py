#!/usr/bin/env python3
"""Real C wrapper tests, isolated source mutations; no live workspace edits."""
from pathlib import Path
import shutil,subprocess,json
root=Path(__file__).resolve().parents[1];out=root/'.artifacts/kernel-refactor-stage1';work=out/'uninterruptible-wait-lpr-mutations'
(work/'tests').mkdir(parents=True,exist_ok=True)
(work/'userland/personality/linux').mkdir(parents=True,exist_ok=True)
for p in (root/'userland').iterdir():
 if p.name=='personality':continue
 q=work/'userland'/p.name
 if not q.exists():q.symlink_to(p,target_is_directory=p.is_dir())
for sub in ['include']:
 q=work/'userland/personality'/sub
 if not q.exists():q.symlink_to(root/'userland/personality'/sub,target_is_directory=True)
for sub in ['musl','kobox2']:
 q=work/sub
 if not q.exists():q.symlink_to(root/sub,target_is_directory=True)
shutil.copytree(root/'userland/personality/linux/runtime',work/'userland/personality/linux/runtime',dirs_exist_ok=True)
for name in ['lpr_internal_wait_unit.c','run-lpr-internal-wait-unit.sh','lpr_wait_not_ready_unit.c','run-lpr-wait-not-ready-unit.sh']:
 shutil.copy2(root/'tests'/name,work/'tests'/name)
cases=[('internal-U','lpr_block/client.c','PACHA_WAIT_UNINTERRUPTIBLE','0','run-lpr-internal-wait-unit.sh'),('outward-U','lpr_wait.c','PACHA_FD_WAIT_FOREVER,\n                    0);','PACHA_FD_WAIT_FOREVER,\n                    PACHA_WAIT_UNINTERRUPTIBLE);','run-lpr-wait-not-ready-unit.sh')]
# The bounded graph uses the other FD_WAIT_MANY arm, so mutate that arm too.
results=[]
for label,name,old,new,script in cases:
 p=work/'userland/personality/linux/runtime'/name;original=p.read_text()
 for control in [True,False]:
  changed=original.replace(old,new,1)
  if label=='outward-U':changed=changed.replace('timeout_ticks,\n            0);','timeout_ticks,\n            PACHA_WAIT_UNINTERRUPTIBLE);')
  assert changed!=original
  p.write_text(original if control else changed)
  log=out/('uninterruptible-wait-mutation-'+label+('-control' if control else '')+'.log')
  with log.open('w') as f:r=subprocess.run(['bash',str(work/'tests'/script)],cwd=work,stdout=f,stderr=subprocess.STDOUT,timeout=60)
  text=log.read_text();ok=(r.returncode==0 if control else r.returncode!=0 and 'Assertion' in text)
  results.append(dict(case=label,control=control,exit=r.returncode,expected_outcome=ok,log=log.name))
  print(label,control,'PASS' if ok else 'FAIL',flush=True)
 p.write_text(original)
(out/'uninterruptible-wait-lpr-mutations.json').write_text(json.dumps(results,indent=2)+'\n')
assert all(x['expected_outcome'] for x in results),results
