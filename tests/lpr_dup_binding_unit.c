#include "../userland/personality/linux/runtime/lpr_fd/dup_pipe.c"
#include <assert.h>
#include <stdio.h>

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[16];
static lpr_ofd_t ofds[16];
static lpr_backend_record_t backends[16];
static lpr_filed_backend_t source, target;
static int64_t binding_result;
static unsigned prepared, dropped, bindings;
int64_t lpr_filed_close_posix_locks(const lpr_fd_pin_t *pin)
{
    assert(pin->state==&target && entries[4].ofd_index==entries[3].ofd_index);
    return 0;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin)
{
    lpr_fd_drop_t drop;
    assert(!lpr_fd_table_unpin(&lpr_control_fd_table,pin,&drop));
    if (drop.ready) (void)lpr_backend_finish_drop(&drop);
}

int lpr_control_require_fd(uint64_t fd)
{
    uint16_t flags;
    return fd < 16 && !lpr_fd_table_get_fd_flags(&lpr_control_fd_table, fd, &flags) ? 0 : -LPR_LINUX_EBADF;
}
int64_t lpr_filed_fd_binding(uint64_t fd)
{ assert(fd == 4); ++bindings; if (!binding_result) target.handle = 522; return binding_result; }
int lpr_fd_table_ensure_fd(uint64_t fd) { assert(fd == 4); return 0; }
int lpr_fd_table_ensure_capacity(uint64_t capacity) { (void)capacity; assert(0); return -1; }
int64_t lpr_fd_prepare_dup(uint64_t fd) { assert(fd == 3); ++prepared; return 0; }
int64_t lpr_backend_finish_drop(const lpr_fd_drop_t *drop)
{ assert(drop->ready && drop->state == &target && target.handle == 522); ++dropped; return 0; }

int main(void)
{
    const int64_t cases[] = {-LPR_LINUX_ENOMEM, -LPR_LINUX_EIO, 0};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        lpr_fd_table_init(&lpr_control_fd_table, entries, 16, ofds, 16, backends, 16);
        lpr_fd_table_capacity = 16;
        source = (lpr_filed_backend_t){.active = 1, .handle = 112};
        target = (lpr_filed_backend_t){.active = 1, .handle = 223};
        lpr_fd_install_t install = {.ops_id = LPR_FD_OPS_FILED, .rights = LPR_FD_RIGHT_DUP,
            .backend_state = &source, .backend_state_bytes = sizeof(source)};
        assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install));
        install.backend_state = &target; install.fd_flags = LPR_FD_ENTRY_CLOEXEC;
        assert(!lpr_fd_table_install_at(&lpr_control_fd_table, 4, &install));
        const uint32_t old_index = entries[4].ofd_index;
        const uint32_t old_generation = entries[4].ofd_generation;
        binding_result = cases[i]; prepared = dropped = bindings = 0;
        assert(lpr_linux_dup_into(3, 4, 0, 0) == (cases[i] ? cases[i] : 4));
        assert(bindings == 1);
        if (cases[i]) {
            assert(!prepared && !dropped && target.handle == 223);
            assert(entries[4].ofd_index == old_index && entries[4].ofd_generation == old_generation);
            assert(entries[4].fd_flags == LPR_FD_ENTRY_CLOEXEC);
        } else {
            assert(prepared == 1 && dropped == 1);
            assert(!lpr_fd_table_compare_ofd(&lpr_control_fd_table, 3, 4));
        }
    }
    puts("LPR_DUP_BINDING_UNIT_PASS");
}
