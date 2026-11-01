#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=.artifacts/tests/lpr-dmabuf-transfer
mkdir -p "$out"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
s=Path('userland/personality/linux/runtime/lpr_fd/ops.c').read_text()
def function(signature):
 a=s.index(signature); b=s.index('{',a); n=1; e=b+1
 while n:
  n+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
a=s.index('    if (pin->ops_id == LPR_FD_OPS_DMABUF) {');b=s.index('    if (pin->ops_id == LPR_FD_OPS_SYNC_FILE)',a)
branch=s[a:b]
start=s.index('        } else if (item->provider == LPR_FD_OPS_DMABUF) {');end=s.index('        } else if (item->provider == LPR_FD_OPS_SYNC_FILE)',start)
validation=s[start:end].split('{',1)[1]
start=s.index('        } else if (item->provider == LPR_FD_OPS_DMABUF) {',end);end=s.index('        } else {',start)
assignment=s[start:end].split('{',1)[1]
output=function('static pacha_fd_t lpr_transfer_duplicate_capability(')+'\n'+function('static int64_t lpr_ops_dmabuf_close(')+'\n'
output+='''static int prepare(const lpr_fd_pin_t *pin, struct unix_transfer_item *item,
 pacha_fd_t *capability_fds,uint32_t capability_capacity,uint32_t *out_capability_count) {
 memset(item,0,sizeof(*item)); *out_capability_count=0;
 item->provider=pin->ops_id;item->rights=pin->effective_rights;
'''+branch+'return -LPR_LINUX_EOPNOTSUPP;\n}\n'
output+='''static int stage(const struct unix_transfer_item *item, pacha_fd_t *item_capabilities,
 lpr_dmabuf_backend_t *out) {
 int status=0; uint64_t handle=item->object,linux_flags=item->flags;
 unsigned prepared_count=0; uint64_t state_bytes=sizeof(*out); void *states[1]={out};
 do {\n'''+validation+'''\n}while(0);
 if(status)return status;
 do {\n'''+assignment+'''\n}while(0);
 return status;
}\n'''
Path(sys.argv[1],'dmabuf_transfer_actual.inc').write_text(output)
PY
includes=()
for module in libipc libpacha personality daemons/common filed termd gpud inputd netd unixd lpr_supervisor; do
 includes+=("-Iuserland/$module/include")
done
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 "${includes[@]}" -Iuserland -Imusl/pachaos/include -Ikobox2/protocol/include \
 -Ikobox2/protocol/generated/include -I"$out" tests/lpr_dmabuf_transfer_unit.c -o "$out/unit"
"$out/unit"
