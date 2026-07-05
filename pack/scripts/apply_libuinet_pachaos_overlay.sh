#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tree="${1:?libuinet source tree path required}"
overlay="${repo_root}/third_party/libuinet/pachaos_overlay"

cp "${overlay}/lib/libuinet/api_include/uinet_pachaos_api.h" \
  "${tree}/lib/libuinet/api_include/uinet_pachaos_api.h"
cp "${overlay}/lib/libuinet/uinet_if_pachaos.c" \
  "${tree}/lib/libuinet/uinet_if_pachaos.c"
cp "${overlay}/lib/libuinet/uinet_if_pachaos.h" \
  "${tree}/lib/libuinet/uinet_if_pachaos.h"
cp "${overlay}/lib/libuinet/uinet_pachaos_host.c" \
  "${tree}/lib/libuinet/uinet_pachaos_host.c"

api_types="${tree}/lib/libuinet/api_include/uinet_api_types.h"
api_h="${tree}/lib/libuinet/api_include/uinet_api.h"
api_c="${tree}/lib/libuinet/uinet_api.c"
config_c="${tree}/lib/libuinet/uinet_config.c"
makefile="${tree}/lib/libuinet/Makefile"
symlist="${tree}/lib/libuinet/uinet_api.symlist"
timeout_c="${tree}/lib/libuinet/uinet_kern_timeout.c"
init_c="${tree}/lib/libuinet/uinet_init.c"
kthread_c="${tree}/lib/libuinet/uinet_kern_kthread.c"
igmp_c="${tree}/sys/netinet/igmp.c"

if ! grep -q "UINET_IFTYPE_PACHAOS" "${api_types}"; then
  perl -0pi -e 's/(\tUINET_IFTYPE_PCAP,\n)/$1\tUINET_IFTYPE_PACHAOS,\n/' "${api_types}"
  perl -0pi -e 's/(struct uinet_if_pcap_cfg \{\n.*?\n\};\n)/$1\nstruct uinet_if_pachaos_cfg {\n\tunsigned char mac[6];\n\tunsigned int mtu;\n};\n/s' "${api_types}"
  perl -0pi -e 's/(struct uinet_if_pcap_cfg pcap;\n)/$1\tstruct uinet_if_pachaos_cfg pachaos;\n/' "${api_types}"
  perl -0pi -e 's/(\t \*  UINET_IFTYPE_PCAP - <hostifname> or file:\/\/<filename>\n)/$1\t *\n\t *  UINET_IFTYPE_PACHAOS - in-process PachaOS netd packet source\n/' "${api_types}"
fi

if ! grep -q 'uinet_if_pachaos.h' "${config_c}"; then
  perl -0pi -e 's/(#include "uinet_if_pcap.h"\n)/$1#include "uinet_if_pachaos.h"\n/' "${config_c}"
fi

if ! grep -q 'if_pachaos_attach' "${config_c}"; then
  perl -0pi -e 's/(\tcase UINET_IFTYPE_PCAP:\n\t\terror = if_pcap_attach\(new_uif\);\n\t\tbreak;\n)/$1\tcase UINET_IFTYPE_PACHAOS:\n\t\terror = if_pachaos_attach(new_uif);\n\t\tbreak;\n/' "${config_c}"
  perl -0pi -e 's/(\tcase UINET_IFTYPE_PCAP:\n\t\terror = if_pcap_detach\(uif\);\n\t\tbreak;\n)/$1\tcase UINET_IFTYPE_PACHAOS:\n\t\terror = if_pachaos_detach(uif);\n\t\tbreak;\n/' "${config_c}"
fi

if ! grep -q 'PACHAOS_LIBUINET.*UINET_IFTYPE_NETMAP' "${config_c}"; then
  perl -0pi -e 's/(\tcase UINET_IFTYPE_NETMAP:\n\t\terror = if_netmap_attach\(new_uif\);\n\t\tbreak;\n\tcase UINET_IFTYPE_PCAP:\n\t\terror = if_pcap_attach\(new_uif\);\n\t\tbreak;\n)/#ifndef PACHAOS_LIBUINET\n$1#endif\n/s' "${config_c}"
  perl -0pi -e 's/(\tcase UINET_IFTYPE_NETMAP:\n\t\terror = if_netmap_detach\(uif\);\n\t\tbreak;\n\tcase UINET_IFTYPE_PCAP:\n\t\terror = if_pcap_detach\(uif\);\n\t\tbreak;\n)/#ifndef PACHAOS_LIBUINET\n$1#endif\n/s' "${config_c}"
fi

if ! grep -q 'uinet_if_pachaos.c' "${makefile}"; then
  perl -0pi -e 's/(\tuinet_if_pcap\.c\t\t\\\n)/$1\tuinet_if_pachaos.c\t\\\n/' "${makefile}"
fi

for symbol in uinet_pachaos_if_register_tx uinet_pachaos_if_deliver; do
  if ! grep -qx "${symbol}" "${symlist}"; then
    printf '%s\n' "${symbol}" >>"${symlist}"
  fi
done

if ! grep -q 'uinet_route_add_default' "${api_h}"; then
  perl -0pi -e 's/(int   uinet_interface_add_alias\(uinet_instance_t uinst, const char \*name, const char \*addr, const char \*braddr, const char \*mask\);\n)/$1int   uinet_route_add_default(uinet_instance_t uinst, const char *gateway);\n/' "${api_h}"
fi

if ! grep -q '#include <net/route.h>' "${api_c}"; then
  perl -0pi -e 's/(#include <net\/if_promiscinet\.h>\n)/$1#include <net\/route.h>\n/' "${api_c}"
fi

if ! grep -q 'uinet_route_add_default' "${api_c}"; then
  perl -0pi -e 's/(int\nuinet_interface_add_alias\(uinet_instance_t uinst, const char \*name,\n)/int\nuinet_route_add_default(uinet_instance_t uinst, const char *gateway)\n{\n\tstruct sockaddr_in dst;\n\tstruct sockaddr_in gw;\n\tstruct sockaddr_in mask;\n\tint error;\n\n\tmemset(\&dst, 0, sizeof(dst));\n\tmemset(\&gw, 0, sizeof(gw));\n\tmemset(\&mask, 0, sizeof(mask));\n\n\tdst.sin_len = sizeof(dst);\n\tdst.sin_family = AF_INET;\n\tgw.sin_len = sizeof(gw);\n\tgw.sin_family = AF_INET;\n\tmask.sin_len = sizeof(mask);\n\tmask.sin_family = AF_INET;\n\n\tif (inet_pton(AF_INET, gateway, \&gw.sin_addr) <= 0)\n\t\treturn (EAFNOSUPPORT);\n\n\tCURVNET_SET(uinst->ui_vnet);\n\terror = rtrequest(RTM_ADD,\n\t    (struct sockaddr *)\&dst,\n\t    (struct sockaddr *)\&gw,\n\t    (struct sockaddr *)\&mask,\n\t    RTF_UP | RTF_GATEWAY | RTF_STATIC,\n\t    NULL);\n\tCURVNET_RESTORE();\n\n\treturn (error == EEXIST ? 0 : error);\n}\n\n\n$1/' "${api_c}"
fi

if ! grep -qx 'uinet_route_add_default' "${symlist}"; then
  printf '%s\n' uinet_route_add_default >>"${symlist}"
fi

if ! grep -q 'PACHAOS_ONLY' "${makefile}"; then
  perl -0pi -e 's/(UINET_SRCS\+=\t\t\t\\\n)/ifdef PACHAOS_ONLY\nCFLAGS+= -DPACHAOS_LIBUINET=1 -fPIE -Wno-dangling-pointer -Wno-array-parameter -Wno-address-of-packed-member -Wno-misleading-indentation -Wno-unused-function -Wno-missing-prototypes -Wno-cast-qual\nCONF_CFLAGS+= -fPIE\nMK_SSP=no\nendif\n\n$1/' "${makefile}"
  perl -0pi -e 's/(UINET_HOST_SRCS\+=\t\t\\\n\tuinet_api_errno\.c\t\\\n)/ifdef PACHAOS_ONLY\nUINET_HOST_SRCS+=\t\t\\\n\tuinet_api_errno.c\t\\\n\tuinet_pachaos_host.c\nelse\n$1/' "${makefile}"
  perl -0pi -e 's/(ifneq \(\$\{HOST_OS\},Darwin\)\nUINET_HOST_SRCS\+= uinet_if_netmap_host\.c\nendif\n)/$1\nendif\n/s' "${makefile}"
  perl -0pi -e 's/(HOST_SRCS = \$\{UINET_HOST_SRCS\}\n)/ifdef PACHAOS_ONLY\nUINET_SRCS := \$(filter-out uinet_if_pcap.c uinet_if_netmap.c,\$(UINET_SRCS))\nendif\n\n$1/' "${makefile}"
fi

if ! grep -q 'PACHAOS_LIBUINET.*softclock_ih' "${timeout_c}"; then
  perl -0pi -e 's/(\tif \(kthread_add\(timer_intr, cc, NULL, \(void \*\)&softclock_ih, 0, 0, "clock"\)\)\n\t\tpanic\("died while creating standard software ithreads"\);\n\n\tcc->cc_cookie = softclock_ih;\n)/#ifdef PACHAOS_LIBUINET\n\tsoftclock_ih = NULL;\n\tcc->cc_cookie = NULL;\n#else\n$1#endif\n/s' "${timeout_c}"
fi

if ! grep -q 'PACHAOS_LIBUINET.*skip startup sleep' "${init_c}"; then
  perl -0pi -e 's/(\tif_netmap_num_extra_bufs = cfg->netmap_extra_bufs;\n)/#ifndef PACHAOS_LIBUINET\n$1#else\n\tif_netmap_num_extra_bufs = 0;\n#endif\n/s' "${init_c}"
  perl -0pi -e 's/(\tsleep\(1\);\n)/#ifndef PACHAOS_LIBUINET\n\t$1#else\n\t\/\* PACHAOS_LIBUINET: skip startup sleep while netd runs in boot path. \*\/\n#endif\n/s' "${init_c}"
fi

if ! grep -q 'PACHAOS_LIBUINET.*single-service stub thread' "${kthread_c}"; then
  perl -0pi -e 's/(\terror = uhi_thread_create\(&host_thread, tsa, pages \* PAGE_SIZE\); \n\n \tmtx_lock\(&notice\.lock\);\n\twhile \(!notice\.utd\)\n\t\tcv_wait\(&notice\.cond, &notice\.lock\);\n\tmtx_unlock\(&notice\.lock\);)/$1/s' "${kthread_c}"
  perl -0pi -e 's/(\terror = uhi_thread_create\(&host_thread, tsa, pages \* PAGE_SIZE\); \n\n \tmtx_lock\(&notice\.lock\);)/\terror = uhi_thread_create\(\&host_thread, tsa, pages * PAGE_SIZE\); \n#ifdef PACHAOS_LIBUINET\n\tif \(error != 0\) {\n\t\t\/\* PACHAOS_LIBUINET: single-service stub thread; no host thread is created. \*\/\n\t\tnotice.utd = utd;\n\t\terror = 0;\n\t}\n#endif\n\n \tmtx_lock\(\&notice.lock\);/g' "${kthread_c}"
fi

if ! grep -q 'PACHAOS_LIBUINET.*skip igmp sysinit' "${igmp_c}"; then
  perl -0pi -e 's/(SYSINIT\(igmp_init, SI_SUB_PSEUDO, SI_ORDER_MIDDLE, igmp_init, NULL\);\n)/#ifndef PACHAOS_LIBUINET\n$1#endif\n\/\* PACHAOS_LIBUINET: skip igmp sysinit for netd bootstrap. \*\/\n/s' "${igmp_c}"
  perl -0pi -e 's/(VNET_SYSINIT\(igmp_init, SI_SUB_PSEUDO, SI_ORDER_MIDDLE, igmp_vnet_init, NULL\);\n)/#ifndef PACHAOS_LIBUINET\n$1#endif\n/s' "${igmp_c}"
  perl -0pi -e 's/(VNET_SYSINIT\(vnet_igmp_init, SI_SUB_PSEUDO, SI_ORDER_ANY, vnet_igmp_init,\n    NULL\);\n)/#ifndef PACHAOS_LIBUINET\n$1#endif\n/s' "${igmp_c}"
fi

printf 'applied PachaOS libuinet overlay to %s\n' "${tree}"
