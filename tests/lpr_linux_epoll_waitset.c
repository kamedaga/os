/* Exercise the existing wait/edge/lifetime cases with networking enabled.
 * The original mixed test requires connect(10.3.0.1) to fail ENETUNREACH,
 * which is not true for the routed NIC used by accelerated desktop tests. */
#define main lpr_epoll_original_main
#include "../userland/fixtures/src/wsl_musl/lpr_epoll_smoke.c"
#undef main

int main(int argc, char **argv)
{
    if (argc != 1) return lpr_epoll_original_main(argc, argv);
    emit("LPR_EPOLL_WAITSET_START\n");
    if (!eventfd_contention_smoke() ||
        !edge_triggered_smoke() ||
        !timeout_dup_close_smoke() ||
        !hup_smoke() ||
        !nested_smoke() ||
        !fork_infinite_smoke() ||
        !cloexec_exec_smoke(argv[0])) return 1;
    emit("LPR_EPOLL_WAITSET_DONE\n");
    return 0;
}
