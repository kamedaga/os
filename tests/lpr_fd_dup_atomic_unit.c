#include <assert.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/lpr_fd/table.h"

int main(void)
{
    lpr_fd_table_t table;
    lpr_fd_entry_t entries[16];
    lpr_ofd_t ofds[16];
    lpr_backend_record_t backends[16];
    int a, b;
    lpr_fd_table_init(&table, entries, 16, ofds, 16, backends, 16);
    lpr_fd_install_t install = { .ops_id = LPR_FD_OPS_DEVICE,
        .rights = LPR_FD_RIGHT_DUP, .backend_state = &a, .backend_state_bytes = sizeof(a) };
    assert(!lpr_fd_table_install_at(&table, 3, &install));
    install.backend_state = &b;
    assert(!lpr_fd_table_install_at(&table, 4, &install));
    const lpr_linux_fd_t excluded[] = {5, 6};
    lpr_linux_fd_t copy;
    assert(!lpr_fd_table_dup_excluding(&table, 3, 5, LPR_FD_ENTRY_CLOEXEC,
        excluded, 2, &copy) && copy == 7);
    assert(entries[7].fd_flags == LPR_FD_ENTRY_CLOEXEC);
    lpr_fd_pin_t pin;
    assert(!lpr_fd_table_pin(&table, 4, &pin));
    lpr_fd_drop_t drop;
    assert(!lpr_fd_table_dup_replace(&table, 3, 4, 0, &drop) && !drop.ready);
    assert(entries[3].ofd_index == entries[4].ofd_index);
    assert(!lpr_fd_table_unpin(&table, &pin, &drop) && drop.ready && drop.state == &b);
    /* Replacing an alias must not destroy its shared OFD. */
    assert(!lpr_fd_table_dup_replace(&table, 3, 4, LPR_FD_ENTRY_CLOEXEC, &drop));
    assert(!drop.ready && ofds[entries[3].ofd_index].refcount == 3);
    assert(lpr_fd_table_dup_replace(&table, 15, 4, 0, &drop) < 0);
    assert(entries[4].active && entries[4].fd_flags == LPR_FD_ENTRY_CLOEXEC);
    assert(!lpr_fd_table_stage_batch(&table, &install, 1, &copy));
    assert(lpr_fd_table_dup_replace(&table, 3, copy, 0, &drop) < 0);
    assert(!lpr_fd_table_abort_staged(&table, copy, &drop));
    assert(!lpr_fd_table_close(&table, 3, &drop) && !drop.ready);
    assert(!lpr_fd_table_close(&table, 4, &drop) && !drop.ready);
    assert(!lpr_fd_table_close(&table, 7, &drop) && drop.ready && drop.state == &a);
    puts("atomic dup allocation/replacement, pinned cleanup, alias and reserved target: PASS");
}
