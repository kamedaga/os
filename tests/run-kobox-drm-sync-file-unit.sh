#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
out=.artifacts/tests/kobox-drm-sync-file
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
parts=[]
files={
 'drm_file.c': ['static bool valid_context(', 'static int check_owner(', 'static int private_ioctl(', 'int kobox_linux_drm_syncobj_export_file(', 'int kobox_linux_drm_syncobj_import_file(', 'static int dmabuf_file_ioctl(', 'int kobox_linux_drm_dmabuf_export_file(', 'int kobox_linux_drm_dmabuf_import_file('],
 'drm_service.c': ['static int check_owner(', 'static struct drm_owned_file *find_file(', 'static struct drm_owned_sync_file *find_sync_file(', 'static int sync_file_reserve(', 'static int retain_sync_file(', 'int kobox_linux_drm_service_syncobj_export_file(', 'int kobox_linux_drm_service_prime_export_file(', 'int kobox_linux_drm_service_syncobj_import_file(', 'int kobox_linux_drm_service_prime_import_file(', 'int kobox_linux_drm_service_release_sync_file(', 'int kobox_linux_drm_service_quiesce(']}
for name,functions in files.items():
 source=Path('kobox2/linux-sandbox/kobox/boot',name).read_text()
 for signature in functions:
  start=source.index(signature);end=source.index('{',start)+1;depth=1
  while depth:
   depth+=(source[end]=='{')-(source[end]=='}');end+=1
  parts.append(source[start:end].replace('check_owner(', 'file_check_owner(' if name=='drm_file.c' else 'service_check_owner('))
Path(sys.argv[1],'sync_file_actual.inc').write_text('\n\n'.join(parts)+'\n')
PY
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -D__EXPORTED_HEADERS__ -D__user= -I "$out" -I kobox2/linux-sandbox/include/uapi \
 tests/kobox_drm_sync_file_unit.c -o "$out/unit"
"$out/unit" | tee "$out/result.log"
