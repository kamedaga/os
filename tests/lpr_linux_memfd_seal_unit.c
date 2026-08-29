#include <stdint.h>
#include <stdio.h>

#include "lpr_fd/memfd.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    uint8_t state = LPR_FILED_FD_MEMFD | LPR_FILED_FD_ALLOW_SEALING;
    const uint64_t glycin_seals =
        LPR_LINUX_F_SEAL_SHRINK |
        LPR_LINUX_F_SEAL_GROW |
        LPR_LINUX_F_SEAL_WRITE |
        LPR_LINUX_F_SEAL_SEAL;

    CHECK(!lpr_memfd_write_is_sealed(state));
    CHECK(lpr_memfd_add_seals(&state, glycin_seals) == 0);
    CHECK((state & LPR_FILED_FD_SEALS) == glycin_seals);
    CHECK(lpr_memfd_write_is_sealed(state));
    CHECK(lpr_memfd_add_seals(&state, LPR_LINUX_F_SEAL_FUTURE_WRITE) < 0);

    state = LPR_FILED_FD_MEMFD | LPR_FILED_FD_ALLOW_SEALING |
        LPR_LINUX_F_SEAL_FUTURE_WRITE;
    CHECK(lpr_memfd_write_is_sealed(state));

    state = LPR_FILED_FD_MEMFD;
    CHECK(lpr_memfd_add_seals(&state, LPR_LINUX_F_SEAL_WRITE) < 0);
    CHECK(lpr_memfd_add_seals(&state, 0x20u) < 0);

    puts("lpr linux memfd seal unit: PASS");
    return 0;
}
