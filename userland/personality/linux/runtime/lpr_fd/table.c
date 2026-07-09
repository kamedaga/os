#include "table.h"

static void lpr_fd_table_zero(void *ptr, uint64_t size)
{
    uint8_t *bytes = (uint8_t *)ptr;
    while (size != 0) {
        *bytes++ = 0;
        size--;
    }
}

static int lpr_fd_table_lock_enabled(const lpr_fd_table_t *table)
{
    return table != 0 &&
        table->lock.thread_count != 0 &&
        __atomic_load_n(table->lock.thread_count, __ATOMIC_RELAXED) > 1u;
}

void lpr_fd_table_configure_lock(
    lpr_fd_table_t *table,
    const volatile uint32_t *thread_count,
    lpr_futex_wait_fn futex_wait,
    lpr_futex_wake_fn futex_wake)
{
    if (table == 0) {
        return;
    }
    table->lock.word = 0;
    table->lock.thread_count = thread_count;
    table->lock.futex_wait = futex_wait;
    table->lock.futex_wake = futex_wake;
}

void lpr_fd_table_lock(lpr_fd_table_t *table)
{
    if (!lpr_fd_table_lock_enabled(table)) {
        return;
    }
    while (!__atomic_compare_exchange_n(
        &table->lock.word,
        &(uint32_t){0},
        1u,
        0,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED))
    {
        if (table->lock.futex_wait != 0) {
            table->lock.futex_wait(&table->lock.word, 1u);
        }
    }
}

void lpr_fd_table_unlock(lpr_fd_table_t *table)
{
    if (!lpr_fd_table_lock_enabled(table)) {
        return;
    }
    __atomic_store_n(&table->lock.word, 0u, __ATOMIC_RELEASE);
    if (table->lock.futex_wake != 0) {
        table->lock.futex_wake(&table->lock.word, 1u);
    }
}

static int lpr_fd_table_valid(const lpr_fd_table_t *table)
{
    return table != 0 &&
        table->slots != 0 &&
        table->files != 0 &&
        table->slot_count != 0 &&
        table->file_count != 0;
}

static lpr_fd_table_slot_t *lpr_fd_table_slot(lpr_fd_table_t *table, uint32_t fd)
{
    if (!lpr_fd_table_valid(table) || fd >= table->slot_count) {
        return 0;
    }
    return &table->slots[fd];
}

static const lpr_fd_table_slot_t *lpr_fd_table_slot_const(const lpr_fd_table_t *table, uint32_t fd)
{
    if (!lpr_fd_table_valid(table) || fd >= table->slot_count) {
        return 0;
    }
    return &table->slots[fd];
}

static lpr_fd_table_file_t *lpr_fd_table_file(lpr_fd_table_t *table, uint32_t index)
{
    if (!lpr_fd_table_valid(table) || index >= table->file_count) {
        return 0;
    }
    return &table->files[index];
}

static const lpr_fd_table_file_t *lpr_fd_table_file_const(
    const lpr_fd_table_t *table,
    uint32_t index)
{
    if (!lpr_fd_table_valid(table) || index >= table->file_count) {
        return 0;
    }
    return &table->files[index];
}

static lpr_fd_table_file_t *lpr_fd_table_file_for_fd(lpr_fd_table_t *table, uint32_t fd)
{
    lpr_fd_table_slot_t *slot = lpr_fd_table_slot(table, fd);
    if (slot == 0 || !slot->active) {
        return 0;
    }
    lpr_fd_table_file_t *file = lpr_fd_table_file(table, slot->file_index);
    if (file == 0 || !file->active) {
        return 0;
    }
    return file;
}

static void lpr_fd_table_fill_payload(
    lpr_fd_table_file_t *file,
    const lpr_fd_table_install_t *install)
{
    if (file == 0 || install == 0) {
        return;
    }
    switch (install->kind) {
    case LPR_FD_TABLE_KIND_FILED:
        file->payload.filed.active = 1;
        file->payload.filed.offset_valid = 1;
        file->payload.filed.flags =
            (install->status_flags & LPR_FD_TABLE_STATUS_NONBLOCK ? 00004000u : 0u) |
            (install->status_flags & LPR_FD_TABLE_STATUS_APPEND ? 00002000u : 0u);
        file->payload.filed.handle = install->backend_id;
        file->payload.filed.offset = install->offset;
        break;
    case LPR_FD_TABLE_KIND_TTY:
        file->payload.tty.active = 1;
        file->payload.tty.handle = install->backend_id;
        break;
    case LPR_FD_TABLE_KIND_PIPE:
        file->payload.pipe.active = 1;
        file->payload.pipe.flags =
            (install->status_flags & LPR_FD_TABLE_STATUS_NONBLOCK ? 00004000u : 0u);
        break;
    case LPR_FD_TABLE_KIND_EVENT:
        file->payload.eventfd.active = 1;
        file->payload.eventfd.counter = install->offset;
        file->payload.eventfd.flags =
            (install->status_flags & LPR_FD_TABLE_STATUS_NONBLOCK ? 00004000u : 0u);
        break;
    case LPR_FD_TABLE_KIND_SOCKET:
        file->payload.socket.active = 1;
        file->payload.socket.handle = install->backend_id;
        file->payload.socket.flags =
            00000002u |
            (install->status_flags & LPR_FD_TABLE_STATUS_NONBLOCK ? 00004000u : 0u);
        file->payload.socket.cloexec =
            (install->fd_flags & LPR_FD_TABLE_FD_CLOEXEC) != 0 ? 1u : 0u;
        break;
    default:
        break;
    }
}

static const lpr_fd_table_file_t *lpr_fd_table_file_for_fd_const(
    const lpr_fd_table_t *table,
    uint32_t fd)
{
    const lpr_fd_table_slot_t *slot = lpr_fd_table_slot_const(table, fd);
    if (slot == 0 || !slot->active) {
        return 0;
    }
    const lpr_fd_table_file_t *file = lpr_fd_table_file_const(table, slot->file_index);
    if (file == 0 || !file->active) {
        return 0;
    }
    return file;
}

static int lpr_fd_table_alloc_file(lpr_fd_table_t *table, uint32_t *out_index)
{
    if (!lpr_fd_table_valid(table) || out_index == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < table->file_count; i++) {
        if (!table->files[i].active) {
            lpr_fd_table_zero(&table->files[i], sizeof(table->files[i]));
            table->files[i].active = 1;
            table->files[i].refcount = 1;
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

void lpr_fd_table_init(
    lpr_fd_table_t *table,
    lpr_fd_table_slot_t *slots,
    uint32_t slot_count,
    lpr_fd_table_file_t *files,
    uint32_t file_count)
{
    if (table == 0) {
        return;
    }
    table->slots = slots;
    table->slot_count = slot_count;
    table->files = files;
    table->file_count = file_count;
    table->generation = 1;
    lpr_fd_table_zero(&table->lock, sizeof(table->lock));
    if (slots != 0) {
        lpr_fd_table_zero(slots, (uint64_t)slot_count * sizeof(slots[0]));
    }
    if (files != 0) {
        lpr_fd_table_zero(files, (uint64_t)file_count * sizeof(files[0]));
    }
}

static int lpr_fd_table_install_at_unlocked(
    lpr_fd_table_t *table,
    uint32_t fd,
    const lpr_fd_table_install_t *install)
{
    lpr_fd_table_slot_t *slot = lpr_fd_table_slot(table, fd);
    if (slot == 0 || install == 0 || install->kind == LPR_FD_TABLE_KIND_EMPTY || slot->active) {
        return -1;
    }
    uint32_t file_index = 0;
    if (lpr_fd_table_alloc_file(table, &file_index) != 0) {
        return -1;
    }
    lpr_fd_table_file_t *file = &table->files[file_index];
    file->kind = install->kind;
    file->status_flags = install->status_flags;
    file->rights = install->rights;
    file->backend_id = install->backend_id;
    file->offset = install->offset;
    file->generation = ++table->generation;
    lpr_fd_table_fill_payload(file, install);
    slot->active = 1;
    slot->fd_flags = install->fd_flags;
    slot->file_index = file_index;
    table->generation++;
    return 0;
}

int lpr_fd_table_install_at(
    lpr_fd_table_t *table,
    uint32_t fd,
    const lpr_fd_table_install_t *install)
{
    lpr_fd_table_lock(table);
    const int status = lpr_fd_table_install_at_unlocked(table, fd, install);
    lpr_fd_table_unlock(table);
    return status;
}

int lpr_fd_table_alloc(
    lpr_fd_table_t *table,
    uint32_t min_fd,
    const lpr_fd_table_install_t *install,
    uint32_t *out_fd)
{
    lpr_fd_table_lock(table);
    if (!lpr_fd_table_valid(table) || out_fd == 0 || min_fd >= table->slot_count) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    for (uint32_t fd = min_fd; fd < table->slot_count; fd++) {
        if (!table->slots[fd].active) {
            if (lpr_fd_table_install_at_unlocked(table, fd, install) != 0) {
                lpr_fd_table_unlock(table);
                return -1;
            }
            *out_fd = fd;
            lpr_fd_table_unlock(table);
            return 0;
        }
    }
    lpr_fd_table_unlock(table);
    return -1;
}

static int lpr_fd_table_close_unlocked(lpr_fd_table_t *table, uint32_t fd)
{
    lpr_fd_table_slot_t *slot = lpr_fd_table_slot(table, fd);
    if (slot == 0 || !slot->active) {
        return -1;
    }
    lpr_fd_table_file_t *file = lpr_fd_table_file(table, slot->file_index);
    if (file == 0 || !file->active || file->refcount == 0) {
        lpr_fd_table_zero(slot, sizeof(*slot));
        return -1;
    }
    file->refcount--;
    if (file->refcount == 0) {
        lpr_fd_table_zero(file, sizeof(*file));
    }
    lpr_fd_table_zero(slot, sizeof(*slot));
    table->generation++;
    return 0;
}

int lpr_fd_table_close(lpr_fd_table_t *table, uint32_t fd)
{
    lpr_fd_table_lock(table);
    const int status = lpr_fd_table_close_unlocked(table, fd);
    lpr_fd_table_unlock(table);
    return status;
}

int lpr_fd_table_dup(
    lpr_fd_table_t *table,
    uint32_t old_fd,
    uint32_t min_fd,
    uint16_t new_fd_flags,
    uint32_t *out_fd)
{
    lpr_fd_table_lock(table);
    const lpr_fd_table_slot_t *old_slot = lpr_fd_table_slot_const(table, old_fd);
    lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd(table, old_fd);
    if (old_slot == 0 || file == 0 || out_fd == 0 || min_fd >= table->slot_count) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    for (uint32_t fd = min_fd; fd < table->slot_count; fd++) {
        if (!table->slots[fd].active) {
            table->slots[fd].active = 1;
            table->slots[fd].fd_flags = new_fd_flags;
            table->slots[fd].file_index = old_slot->file_index;
            file->refcount++;
            table->generation++;
            *out_fd = fd;
            lpr_fd_table_unlock(table);
            return 0;
        }
    }
    lpr_fd_table_unlock(table);
    return -1;
}

int lpr_fd_table_dup2(
    lpr_fd_table_t *table,
    uint32_t old_fd,
    uint32_t new_fd,
    uint16_t new_fd_flags)
{
    lpr_fd_table_lock(table);
    const lpr_fd_table_slot_t *old_slot = lpr_fd_table_slot_const(table, old_fd);
    lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd(table, old_fd);
    lpr_fd_table_slot_t *new_slot = lpr_fd_table_slot(table, new_fd);
    if (old_slot == 0 || file == 0 || new_slot == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    if (old_fd == new_fd) {
        lpr_fd_table_unlock(table);
        return 0;
    }
    if (new_slot->active && lpr_fd_table_close_unlocked(table, new_fd) != 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    new_slot->active = 1;
    new_slot->fd_flags = new_fd_flags;
    new_slot->file_index = old_slot->file_index;
    file->refcount++;
    table->generation++;
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_close_range(
    lpr_fd_table_t *table,
    uint32_t first,
    uint32_t last,
    uint32_t cloexec_only)
{
    lpr_fd_table_lock(table);
    if (!lpr_fd_table_valid(table) || first > last) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    if (last >= table->slot_count) {
        last = table->slot_count - 1u;
    }
    if (first >= table->slot_count) {
        lpr_fd_table_unlock(table);
        return 0;
    }
    const uint64_t old_generation = table->generation;
    uint32_t changed = 0;
    for (uint32_t fd = first; fd <= last; fd++) {
        if (!table->slots[fd].active) {
            continue;
        }
        if (cloexec_only) {
            table->slots[fd].fd_flags |= LPR_FD_TABLE_FD_CLOEXEC;
            changed = 1;
        } else {
            if (lpr_fd_table_close_unlocked(table, fd) == 0) {
                changed = 1;
            }
        }
    }
    if (changed) {
        table->generation = old_generation + 1u;
    }
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_fd_flags(const lpr_fd_table_t *table, uint32_t fd, uint16_t *out_flags)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_table_slot_t *slot = lpr_fd_table_slot_const(table, fd);
    if (slot == 0 || !slot->active || out_flags == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_flags = slot->fd_flags;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_fd_flags(lpr_fd_table_t *table, uint32_t fd, uint16_t flags)
{
    lpr_fd_table_lock(table);
    lpr_fd_table_slot_t *slot = lpr_fd_table_slot(table, fd);
    if (slot == 0 || !slot->active) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    slot->fd_flags = flags;
    table->generation++;
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_status_flags(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_flags)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd_const(table, fd);
    if (file == 0 || out_flags == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_flags = file->status_flags;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_status_flags(lpr_fd_table_t *table, uint32_t fd, uint32_t flags)
{
    lpr_fd_table_lock(table);
    lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd(table, fd);
    if (file == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    file->status_flags = flags;
    table->generation++;
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_offset(const lpr_fd_table_t *table, uint32_t fd, uint64_t *out_offset)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd_const(table, fd);
    if (file == 0 || out_offset == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_offset = file->offset;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_offset(lpr_fd_table_t *table, uint32_t fd, uint64_t offset)
{
    lpr_fd_table_lock(table);
    lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd(table, fd);
    if (file == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    file->offset = offset;
    table->generation++;
    lpr_fd_table_unlock(table);
    return 0;
}

lpr_fd_object_t *lpr_fd_table_object_for_fd(lpr_fd_table_t *table, uint32_t fd)
{
    lpr_fd_table_lock(table);
    lpr_fd_object_t *object = lpr_fd_table_file_for_fd(table, fd);
    lpr_fd_table_unlock(table);
    return object;
}

const lpr_fd_object_t *lpr_fd_table_object_for_fd_const(const lpr_fd_table_t *table, uint32_t fd)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_object_t *object = lpr_fd_table_file_for_fd_const(table, fd);
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return object;
}

int lpr_fd_table_get_kind(const lpr_fd_table_t *table, uint32_t fd, uint8_t *out_kind)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd_const(table, fd);
    if (file == 0 || out_kind == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_kind = file->kind;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_get_refcount(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_refcount)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_table_file_t *file = lpr_fd_table_file_for_fd_const(table, fd);
    if (file == 0 || out_refcount == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_refcount = file->refcount;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    if (!lpr_fd_table_valid(table)) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->slot_count; i++) {
        if (table->slots[i].active) {
            count++;
        }
    }
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return count;
}

uint32_t lpr_fd_table_live_file_count(const lpr_fd_table_t *table)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    if (!lpr_fd_table_valid(table)) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->file_count; i++) {
        if (table->files[i].active) {
            count++;
        }
    }
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return count;
}
