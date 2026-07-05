#include "netd_internal.h"

#if defined(NETD_WITH_LIBUINET)
int rand(void)
{
    static unsigned state;
    state ^= (unsigned)netd_metrics_read_tsc();
    state = state * 1103515245u + 12345u;
    return (int)((state >> 1) & 0x7fffffffu);
}
#endif
