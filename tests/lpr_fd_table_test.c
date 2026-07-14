#include "lpr_fd/table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        abort(); \
    } \
} while (0)

static uint32_t futex_wait_calls;
static uint32_t futex_wake_calls;

static void fake_futex_wait(volatile uint32_t *word, uint32_t expected)
{
    CHECK(expected == 1u);
    futex_wait_calls++;
    *word = 0;
}

static void fake_futex_wake(volatile uint32_t *word, uint32_t count)
{
    CHECK(*word == 0u);
    CHECK(count == 1u);
    futex_wake_calls++;
}

static void test_dup_shares_file_object(void)
{
    lpr_fd_table_slot_t slots[8];
    lpr_fd_table_file_t files[8];
    lpr_fd_table_t table;
    lpr_fd_table_init(&table, slots, 8, files, 8);

    const lpr_fd_table_install_t install = {
        .kind = LPR_FD_TABLE_KIND_FILED,
        .fd_flags = LPR_FD_TABLE_FD_CLOEXEC,
        .status_flags = LPR_FD_TABLE_STATUS_NONBLOCK,
        .rights = 0x55u,
        .backend_id = 1234u,
        .offset = 7u,
    };

    CHECK(lpr_fd_table_install_at(&table, 3, &install) == 0);
    uint32_t dup_fd = 0;
    CHECK(lpr_fd_table_dup(&table, 3, 4, 0, &dup_fd) == 0);
    CHECK(dup_fd == 4);
    CHECK(lpr_fd_table_open_count(&table) == 2);
    CHECK(lpr_fd_table_live_file_count(&table) == 1);
    CHECK(files[slots[3].file_index].refcount == 2);
    CHECK(slots[3].file_index == slots[4].file_index);

    uint16_t fd_flags = 0;
    CHECK(lpr_fd_table_get_fd_flags(&table, 3, &fd_flags) == 0);
    CHECK(fd_flags == LPR_FD_TABLE_FD_CLOEXEC);
    CHECK(lpr_fd_table_get_fd_flags(&table, 4, &fd_flags) == 0);
    CHECK(fd_flags == 0);

    uint32_t status_flags = 0;
    CHECK(lpr_fd_table_set_status_flags(&table, 4, LPR_FD_TABLE_STATUS_APPEND) == 0);
    CHECK(lpr_fd_table_get_status_flags(&table, 3, &status_flags) == 0);
    CHECK(status_flags == LPR_FD_TABLE_STATUS_APPEND);

    CHECK(lpr_fd_table_set_offset(&table, 4, 99u) == 0);
    uint64_t offset = 0;
    CHECK(lpr_fd_table_get_offset(&table, 3, &offset) == 0);
    CHECK(offset == 99u);
}

static void test_dup2_closes_target_atomically_in_model(void)
{
    lpr_fd_table_slot_t slots[8];
    lpr_fd_table_file_t files[8];
    lpr_fd_table_t table;
    lpr_fd_table_init(&table, slots, 8, files, 8);

    const lpr_fd_table_install_t filed = {
        .kind = LPR_FD_TABLE_KIND_FILED,
        .backend_id = 10u,
    };
    const lpr_fd_table_install_t tty = {
        .kind = LPR_FD_TABLE_KIND_TTY,
        .backend_id = 20u,
    };

    CHECK(lpr_fd_table_install_at(&table, 1, &filed) == 0);
    CHECK(lpr_fd_table_install_at(&table, 5, &tty) == 0);
    CHECK(lpr_fd_table_live_file_count(&table) == 2);
    CHECK(lpr_fd_table_dup2(&table, 1, 5, LPR_FD_TABLE_FD_CLOEXEC) == 0);
    CHECK(lpr_fd_table_open_count(&table) == 2);
    CHECK(lpr_fd_table_live_file_count(&table) == 1);
    CHECK(slots[1].file_index == slots[5].file_index);
    CHECK(files[slots[1].file_index].refcount == 2);

    uint16_t fd_flags = 0;
    CHECK(lpr_fd_table_get_fd_flags(&table, 5, &fd_flags) == 0);
    CHECK(fd_flags == LPR_FD_TABLE_FD_CLOEXEC);
}

static void test_close_range_cloexec_is_fd_local(void)
{
    lpr_fd_table_slot_t slots[8];
    lpr_fd_table_file_t files[8];
    lpr_fd_table_t table;
    lpr_fd_table_init(&table, slots, 8, files, 8);

    const lpr_fd_table_install_t install = {
        .kind = LPR_FD_TABLE_KIND_PIPE,
        .status_flags = LPR_FD_TABLE_STATUS_NONBLOCK,
    };

    CHECK(lpr_fd_table_install_at(&table, 2, &install) == 0);
    uint32_t dup_fd = 0;
    CHECK(lpr_fd_table_dup(&table, 2, 3, 0, &dup_fd) == 0);
    const uint64_t generation_before = table.generation;
    CHECK(lpr_fd_table_close_range(&table, 3, 7, 1) == 0);
    CHECK(table.generation == generation_before + 1u);

    uint16_t fd_flags = 0;
    CHECK(lpr_fd_table_get_fd_flags(&table, 2, &fd_flags) == 0);
    CHECK(fd_flags == 0);
    CHECK(lpr_fd_table_get_fd_flags(&table, 3, &fd_flags) == 0);
    CHECK(fd_flags == LPR_FD_TABLE_FD_CLOEXEC);

    uint32_t status_flags = 0;
    CHECK(lpr_fd_table_get_status_flags(&table, 2, &status_flags) == 0);
    CHECK(status_flags == LPR_FD_TABLE_STATUS_NONBLOCK);
}

static void test_futex_lock_single_thread_fast_path(void)
{
    lpr_fd_table_slot_t slots[8];
    lpr_fd_table_file_t files[8];
    lpr_fd_table_t table;
    volatile uint32_t thread_count = 1;
    lpr_fd_table_init(&table, slots, 8, files, 8);
    lpr_fd_table_configure_lock(
        &table,
        &thread_count,
        fake_futex_wait,
        fake_futex_wake);

    futex_wait_calls = 0;
    futex_wake_calls = 0;
    const lpr_fd_table_install_t install = {
        .kind = LPR_FD_TABLE_KIND_FILED,
        .backend_id = 42u,
    };
    CHECK(lpr_fd_table_install_at(&table, 3, &install) == 0);
    CHECK(futex_wait_calls == 0u);
    CHECK(futex_wake_calls == 0u);

    thread_count = 2;
    table.lock.word = 1;
    CHECK(lpr_fd_table_install_at(&table, 4, &install) == 0);
    CHECK(futex_wait_calls == 1u);
    CHECK(futex_wake_calls == 1u);
}

static void test_unlock_clears_lock_after_last_peer_exits(void)
{
    lpr_fd_table_slot_t slots[8];
    lpr_fd_table_file_t files[8];
    lpr_fd_table_t table;
    volatile uint32_t thread_count = 2;
    lpr_fd_table_init(&table, slots, 8, files, 8);
    lpr_fd_table_configure_lock(
        &table,
        &thread_count,
        fake_futex_wait,
        fake_futex_wake);

    futex_wake_calls = 0;
    lpr_fd_table_lock(&table);
    CHECK(table.lock.word == 1u);
    thread_count = 1;
    lpr_fd_table_unlock(&table);
    CHECK(table.lock.word == 0u);
    CHECK(futex_wake_calls == 1u);
}

static void test_fcntl_dupfd_shape(void)
{
    lpr_fd_table_slot_t slots[16];
    lpr_fd_table_file_t files[16];
    lpr_fd_table_t table;
    lpr_fd_table_init(&table, slots, 16, files, 16);

    const lpr_fd_table_install_t install = {
        .kind = LPR_FD_TABLE_KIND_FILED,
        .fd_flags = LPR_FD_TABLE_FD_CLOEXEC,
        .status_flags = LPR_FD_TABLE_STATUS_NONBLOCK,
        .backend_id = 8u,
        .offset = 4u,
    };

    CHECK(lpr_fd_table_install_at(&table, 8, &install) == 0);
    uint32_t dup_fd = 0;
    CHECK(lpr_fd_table_dup(&table, 8, 10, 0, &dup_fd) == 0);
    CHECK(dup_fd == 10);
    CHECK(slots[8].file_index == slots[10].file_index);

    uint16_t fd_flags = 0;
    CHECK(lpr_fd_table_get_fd_flags(&table, 10, &fd_flags) == 0);
    CHECK(fd_flags == 0);

    CHECK(lpr_fd_table_set_status_flags(&table, 10, LPR_FD_TABLE_STATUS_APPEND) == 0);
    uint32_t status_flags = 0;
    CHECK(lpr_fd_table_get_status_flags(&table, 8, &status_flags) == 0);
    CHECK(status_flags == LPR_FD_TABLE_STATUS_APPEND);

    CHECK(lpr_fd_table_set_offset(&table, 10, 12u) == 0);
    uint64_t offset = 0;
    CHECK(lpr_fd_table_get_offset(&table, 8, &offset) == 0);
    CHECK(offset == 12u);
}

int main(void)
{
    test_dup_shares_file_object();
    test_dup2_closes_target_atomically_in_model();
    test_close_range_cloexec_is_fd_local();
    test_fcntl_dupfd_shape();
    test_futex_lock_single_thread_fast_path();
    test_unlock_clears_lock_after_last_peer_exits();
    return 0;
}
