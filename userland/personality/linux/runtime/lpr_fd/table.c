#include "table.h"

static void lpr_fd_zero(void *ptr, uint64_t size)
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
        __atomic_load_n(table->lock.thread_count, __ATOMIC_ACQUIRE) > 1u;
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
    if (__atomic_exchange_n(&table->lock.word, 1u, __ATOMIC_ACQUIRE) == 0u) {
        return;
    }
    while (__atomic_exchange_n(&table->lock.word, 2u, __ATOMIC_ACQUIRE) != 0u) {
        if (table->lock.futex_wait != 0) {
            table->lock.futex_wait(&table->lock.word, 2u);
        }
    }
}

void lpr_fd_table_unlock(lpr_fd_table_t *table)
{
    if (table == 0) {
        return;
    }
    if (__atomic_exchange_n(&table->lock.word, 0u, __ATOMIC_RELEASE) == 2u &&
        table->lock.futex_wake != 0)
    {
        table->lock.futex_wake(&table->lock.word, 1u);
    }
}

static int lpr_fd_table_valid(const lpr_fd_table_t *table)
{
    return table != 0 &&
        table->entries != 0 && table->entry_count != 0 &&
        table->ofds != 0 && table->ofd_count != 0 &&
        table->backends != 0 && table->backend_count != 0;
}

static uint32_t lpr_fd_next_generation(lpr_fd_table_t *table)
{
    table->generation++;
    if (table->generation == 0) {
        table->generation = 1;
    }
    return table->generation;
}

static lpr_fd_entry_t *lpr_fd_entry(lpr_fd_table_t *table, lpr_linux_fd_t fd)
{
    if (!lpr_fd_table_valid(table) || fd >= table->entry_count) {
        return 0;
    }
    return &table->entries[fd];
}

static const lpr_fd_entry_t *lpr_fd_entry_const(
    const lpr_fd_table_t *table,
    lpr_linux_fd_t fd)
{
    if (!lpr_fd_table_valid(table) || fd >= table->entry_count) {
        return 0;
    }
    return &table->entries[fd];
}

static lpr_ofd_t *lpr_fd_ofd(lpr_fd_table_t *table, uint32_t index, uint32_t generation)
{
    if (!lpr_fd_table_valid(table) || index >= table->ofd_count) {
        return 0;
    }
    lpr_ofd_t *ofd = &table->ofds[index];
    return ofd->active && ofd->generation == generation ? ofd : 0;
}

static const lpr_ofd_t *lpr_fd_ofd_const(
    const lpr_fd_table_t *table,
    uint32_t index,
    uint32_t generation)
{
    if (!lpr_fd_table_valid(table) || index >= table->ofd_count) {
        return 0;
    }
    const lpr_ofd_t *ofd = &table->ofds[index];
    return ofd->active && ofd->generation == generation ? ofd : 0;
}

static lpr_backend_record_t *lpr_fd_backend(
    lpr_fd_table_t *table,
    lpr_backend_ref_t ref)
{
    if (!lpr_fd_table_valid(table) || ref.index >= table->backend_count) {
        return 0;
    }
    lpr_backend_record_t *backend = &table->backends[ref.index];
    return backend->active && backend->generation == ref.generation ? backend : 0;
}

static lpr_ofd_t *lpr_fd_ofd_for_entry(
    lpr_fd_table_t *table,
    const lpr_fd_entry_t *entry)
{
    if (entry == 0 || !entry->active) {
        return 0;
    }
    return lpr_fd_ofd(table, entry->ofd_index, entry->ofd_generation);
}

static const lpr_ofd_t *lpr_fd_ofd_for_entry_const(
    const lpr_fd_table_t *table,
    const lpr_fd_entry_t *entry)
{
    if (entry == 0 || !entry->active) {
        return 0;
    }
    return lpr_fd_ofd_const(table, entry->ofd_index, entry->ofd_generation);
}

static int lpr_fd_alloc_ofd(lpr_fd_table_t *table, uint32_t *out_index)
{
    for (uint32_t i = 0; i < table->ofd_count; i++) {
        if (!table->ofds[i].active) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

static int lpr_fd_alloc_backend(lpr_fd_table_t *table, uint32_t *out_index)
{
    for (uint32_t i = 0; i < table->backend_count; i++) {
        if (!table->backends[i].active) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

void lpr_fd_table_init(
    lpr_fd_table_t *table,
    lpr_fd_entry_t *entries,
    uint32_t entry_count,
    lpr_ofd_t *ofds,
    uint32_t ofd_count,
    lpr_backend_record_t *backends,
    uint32_t backend_count)
{
    if (table == 0) {
        return;
    }
    lpr_fd_zero(table, sizeof(*table));
    table->entries = entries;
    table->entry_count = entry_count;
    table->ofds = ofds;
    table->ofd_count = ofd_count;
    table->backends = backends;
    table->backend_count = backend_count;
    table->generation = 1;
    if (entries != 0) {
        lpr_fd_zero(entries, (uint64_t)entry_count * sizeof(entries[0]));
    }
    if (ofds != 0) {
        lpr_fd_zero(ofds, (uint64_t)ofd_count * sizeof(ofds[0]));
    }
    if (backends != 0) {
        lpr_fd_zero(backends, (uint64_t)backend_count * sizeof(backends[0]));
    }
}

static int lpr_fd_install_unlocked(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    const lpr_fd_install_t *install)
{
    lpr_fd_entry_t *entry = lpr_fd_entry(table, fd);
    if (entry == 0 || entry->active || install == 0 ||
        install->ops_id == LPR_FD_OPS_NONE || install->ops_id >= LPR_FD_OPS_COUNT ||
        install->backend_state == 0 || install->backend_state_bytes == 0)
    {
        return -1;
    }
    uint32_t ofd_index = 0;
    uint32_t backend_index = 0;
    if (lpr_fd_alloc_ofd(table, &ofd_index) != 0 ||
        lpr_fd_alloc_backend(table, &backend_index) != 0)
    {
        return -1;
    }
    lpr_backend_record_t *backend = &table->backends[backend_index];
    lpr_fd_zero(backend, sizeof(*backend));
    backend->active = 1;
    backend->ops_id = install->ops_id;
    backend->generation = lpr_fd_next_generation(table);
    backend->state = install->backend_state;
    backend->state_bytes = install->backend_state_bytes;

    lpr_ofd_t *ofd = &table->ofds[ofd_index];
    lpr_fd_zero(ofd, sizeof(*ofd));
    ofd->active = 1;
    ofd->access_mode = install->access_mode;
    ofd->refcount = 1;
    ofd->status_flags = install->status_flags;
    ofd->rights_ceiling = install->rights;
    ofd->generation = lpr_fd_next_generation(table);
    ofd->offset = install->offset;
    ofd->backend.index = backend_index;
    ofd->backend.generation = backend->generation;

    lpr_fd_zero(entry, sizeof(*entry));
    entry->active = 1;
    entry->fd_flags = install->fd_flags;
    entry->ofd_index = ofd_index;
    entry->ofd_generation = ofd->generation;
    entry->effective_rights = install->rights;
    (void)lpr_fd_next_generation(table);
    return 0;
}

int lpr_fd_table_install_at(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    const lpr_fd_install_t *install)
{
    lpr_fd_table_lock(table);
    const int status = lpr_fd_install_unlocked(table, fd, install);
    lpr_fd_table_unlock(table);
    return status;
}

int lpr_fd_table_alloc(
    lpr_fd_table_t *table,
    lpr_linux_fd_t min_fd,
    const lpr_fd_install_t *install,
    lpr_linux_fd_t *out_fd)
{
    lpr_fd_table_lock(table);
    if (!lpr_fd_table_valid(table) || out_fd == 0 || min_fd >= table->entry_count) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    for (lpr_linux_fd_t fd = min_fd; fd < table->entry_count; fd++) {
        if (!table->entries[fd].active && lpr_fd_install_unlocked(table, fd, install) == 0) {
            *out_fd = fd;
            lpr_fd_table_unlock(table);
            return 0;
        }
    }
    lpr_fd_table_unlock(table);
    return -1;
}

int lpr_fd_table_alloc_batch(
    lpr_fd_table_t *table,
    lpr_linux_fd_t min_fd,
    const lpr_fd_install_t *installs,
    uint32_t install_count,
    const lpr_linux_fd_t *excluded_fds,
    uint32_t excluded_count,
    lpr_linux_fd_t *out_fds)
{
    if (install_count == 0) return 0;
    lpr_fd_table_lock(table);
    if (!lpr_fd_table_valid(table) || installs == 0 || out_fds == 0 ||
        min_fd >= table->entry_count || install_count > table->entry_count)
    {
        lpr_fd_table_unlock(table);
        return -1;
    }
    uint32_t free_ofds = 0;
    uint32_t free_backends = 0;
    for (uint32_t i = 0; i < table->ofd_count; ++i)
        free_ofds += table->ofds[i].active ? 0u : 1u;
    for (uint32_t i = 0; i < table->backend_count; ++i)
        free_backends += table->backends[i].active ? 0u : 1u;
    if (free_ofds < install_count || free_backends < install_count) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    uint32_t selected = 0;
    for (lpr_linux_fd_t fd = min_fd;
         fd < table->entry_count && selected < install_count;
         ++fd)
    {
        if (table->entries[fd].active) continue;
        int excluded = 0;
        for (uint32_t i = 0; i < excluded_count; ++i)
            if (excluded_fds != 0 && excluded_fds[i] == fd) {
                excluded = 1;
                break;
            }
        if (!excluded) out_fds[selected++] = fd;
    }
    if (selected != install_count) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    for (uint32_t i = 0; i < install_count; ++i) {
        const lpr_fd_install_t *install = &installs[i];
        if (install->ops_id == LPR_FD_OPS_NONE ||
            install->ops_id >= LPR_FD_OPS_COUNT ||
            install->backend_state == 0 || install->backend_state_bytes == 0)
        {
            lpr_fd_table_unlock(table);
            return -1;
        }
    }
    for (uint32_t i = 0; i < install_count; ++i) {
        if (lpr_fd_install_unlocked(table, out_fds[i], &installs[i]) != 0) {
            lpr_fd_table_unlock(table);
            return -1;
        }
    }
    lpr_fd_table_unlock(table);
    return 0;
}

static void lpr_fd_prepare_drop(
    lpr_fd_table_t *table,
    lpr_ofd_t *ofd,
    lpr_fd_drop_t *out_drop)
{
    lpr_backend_record_t *backend = lpr_fd_backend(table, ofd->backend);
    if (out_drop != 0) {
        lpr_fd_zero(out_drop, sizeof(*out_drop));
    }
    if (backend != 0 && out_drop != 0) {
        out_drop->ready = 1;
        out_drop->ops_id = backend->ops_id;
        out_drop->backend_generation = backend->generation;
        out_drop->state = backend->state;
        out_drop->state_bytes = backend->state_bytes;
    }
    if (backend != 0) {
        lpr_fd_zero(backend, sizeof(*backend));
    }
    lpr_fd_zero(ofd, sizeof(*ofd));
    (void)lpr_fd_next_generation(table);
}

int lpr_fd_table_close(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    lpr_fd_drop_t *out_drop)
{
    if (out_drop != 0) {
        lpr_fd_zero(out_drop, sizeof(*out_drop));
    }
    lpr_fd_table_lock(table);
    lpr_fd_entry_t *entry = lpr_fd_entry(table, fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, entry);
    if (entry == 0 || ofd == 0 || ofd->refcount == 0 || ofd->closing) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    lpr_fd_zero(entry, sizeof(*entry));
    ofd->refcount--;
    if (ofd->refcount == 0) {
        ofd->closing = 1;
        if (ofd->pin_count == 0) {
            lpr_fd_prepare_drop(table, ofd, out_drop);
        }
    }
    (void)lpr_fd_next_generation(table);
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_dup(
    lpr_fd_table_t *table,
    lpr_linux_fd_t old_fd,
    lpr_linux_fd_t min_fd,
    uint16_t new_fd_flags,
    lpr_linux_fd_t *out_fd)
{
    lpr_fd_table_lock(table);
    const lpr_fd_entry_t *old_entry = lpr_fd_entry_const(table, old_fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, old_entry);
    if (old_entry == 0 || ofd == 0 || ofd->closing ||
        (old_entry->effective_rights & LPR_FD_RIGHT_DUP) == 0 || out_fd == 0 ||
        min_fd >= table->entry_count)
    {
        lpr_fd_table_unlock(table);
        return -1;
    }
    for (lpr_linux_fd_t fd = min_fd; fd < table->entry_count; fd++) {
        if (!table->entries[fd].active) {
            table->entries[fd] = *old_entry;
            table->entries[fd].fd_flags = new_fd_flags;
            ofd->refcount++;
            (void)lpr_fd_next_generation(table);
            *out_fd = fd;
            lpr_fd_table_unlock(table);
            return 0;
        }
    }
    lpr_fd_table_unlock(table);
    return -1;
}

int lpr_fd_table_dup_at(
    lpr_fd_table_t *table,
    lpr_linux_fd_t old_fd,
    lpr_linux_fd_t new_fd,
    uint16_t new_fd_flags)
{
    lpr_fd_table_lock(table);
    const lpr_fd_entry_t *old_entry = lpr_fd_entry_const(table, old_fd);
    lpr_fd_entry_t *new_entry = lpr_fd_entry(table, new_fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, old_entry);
    if (old_entry == 0 || new_entry == 0 || ofd == 0 || ofd->closing ||
        (old_entry->effective_rights & LPR_FD_RIGHT_DUP) == 0)
    {
        lpr_fd_table_unlock(table);
        return -1;
    }
    if (old_fd == new_fd) {
        lpr_fd_table_unlock(table);
        return 0;
    }
    if (new_entry->active) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    *new_entry = *old_entry;
    new_entry->fd_flags = new_fd_flags;
    ofd->refcount++;
    (void)lpr_fd_next_generation(table);
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_pin(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    lpr_fd_pin_t *out_pin)
{
    if (out_pin == 0) {
        return -1;
    }
    lpr_fd_zero(out_pin, sizeof(*out_pin));
    lpr_fd_table_lock(table);
    const lpr_fd_entry_t *entry = lpr_fd_entry_const(table, fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, entry);
    lpr_backend_record_t *backend = ofd != 0 ? lpr_fd_backend(table, ofd->backend) : 0;
    if (entry == 0 || ofd == 0 || backend == 0 || ofd->closing || ofd->refcount == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    ofd->pin_count++;
    out_pin->fd = fd;
    out_pin->fd_flags = entry->fd_flags;
    out_pin->access_mode = ofd->access_mode;
    out_pin->effective_rights = entry->effective_rights;
    out_pin->status_flags = ofd->status_flags;
    out_pin->ofd_index = entry->ofd_index;
    out_pin->ofd_generation = entry->ofd_generation;
    out_pin->backend_index = ofd->backend.index;
    out_pin->backend_generation = ofd->backend.generation;
    out_pin->ops_id = backend->ops_id;
    out_pin->offset = ofd->offset;
    out_pin->state = backend->state;
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_unpin(
    lpr_fd_table_t *table,
    const lpr_fd_pin_t *pin,
    lpr_fd_drop_t *out_drop)
{
    if (out_drop != 0) {
        lpr_fd_zero(out_drop, sizeof(*out_drop));
    }
    if (pin == 0) {
        return -1;
    }
    lpr_fd_table_lock(table);
    lpr_ofd_t *ofd = lpr_fd_ofd(table, pin->ofd_index, pin->ofd_generation);
    if (ofd == 0 || ofd->backend.index != pin->backend_index ||
        ofd->backend.generation != pin->backend_generation || ofd->pin_count == 0)
    {
        lpr_fd_table_unlock(table);
        return -1;
    }
    ofd->pin_count--;
    if (ofd->closing && ofd->refcount == 0 && ofd->pin_count == 0) {
        lpr_fd_prepare_drop(table, ofd, out_drop);
    }
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_seek_pinned(
    lpr_fd_table_t *table,
    const lpr_fd_pin_t *pin,
    int64_t delta,
    uint32_t whence,
    uint64_t *out_offset)
{
    if (pin == 0 || out_offset == 0 || whence > 1u) {
        return -1;
    }
    lpr_fd_table_lock(table);
    lpr_ofd_t *ofd = lpr_fd_ofd(table, pin->ofd_index, pin->ofd_generation);
    lpr_backend_record_t *backend = ofd != 0 ? lpr_fd_backend(table, ofd->backend) : 0;
    if (ofd == 0 || backend == 0 || ofd->pin_count == 0 ||
        ofd->backend.index != pin->backend_index ||
        ofd->backend.generation != pin->backend_generation ||
        backend->ops_id != pin->ops_id || backend->state != pin->state)
    {
        lpr_fd_table_unlock(table);
        return -1;
    }

    uint64_t next = 0;
    if (whence == 0u) {
        if (delta < 0) {
            lpr_fd_table_unlock(table);
            return -1;
        }
        next = (uint64_t)delta;
    } else if (delta >= 0) {
        const uint64_t amount = (uint64_t)delta;
        if (ofd->offset > UINT64_MAX - amount) {
            lpr_fd_table_unlock(table);
            return -1;
        }
        next = ofd->offset + amount;
    } else {
        const uint64_t amount = 0u - (uint64_t)delta;
        if (amount > ofd->offset) {
            lpr_fd_table_unlock(table);
            return -1;
        }
        next = ofd->offset - amount;
    }

    ofd->offset = next;
    if (backend->ops_id == LPR_FD_OPS_FILED &&
        backend->state_bytes >= sizeof(lpr_filed_backend_t))
    {
        ((lpr_filed_backend_t *)backend->state)->offset = next;
    }
    (void)lpr_fd_next_generation(table);
    *out_offset = next;
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_fd_flags(
    const lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    uint16_t *out_flags)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_entry_t *entry = lpr_fd_entry_const(table, fd);
    if (entry == 0 || !entry->active || out_flags == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_flags = entry->fd_flags;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_fd_flags(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint16_t flags)
{
    lpr_fd_table_lock(table);
    lpr_fd_entry_t *entry = lpr_fd_entry(table, fd);
    if (entry == 0 || !entry->active) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    entry->fd_flags = flags;
    (void)lpr_fd_next_generation(table);
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_status_flags(
    const lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    uint32_t *out_flags)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_entry_t *entry = lpr_fd_entry_const(table, fd);
    const lpr_ofd_t *ofd = lpr_fd_ofd_for_entry_const(table, entry);
    if (ofd == 0 || out_flags == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_flags = ofd->status_flags;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_status_flags(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint32_t flags)
{
    lpr_fd_table_lock(table);
    lpr_fd_entry_t *entry = lpr_fd_entry(table, fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, entry);
    if (ofd == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    ofd->status_flags = flags;
    (void)lpr_fd_next_generation(table);
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_offset(
    const lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    uint64_t *out_offset)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_entry_t *entry = lpr_fd_entry_const(table, fd);
    const lpr_ofd_t *ofd = lpr_fd_ofd_for_entry_const(table, entry);
    if (ofd == 0 || out_offset == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_offset = ofd->offset;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

int lpr_fd_table_set_offset(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint64_t offset)
{
    lpr_fd_table_lock(table);
    lpr_fd_entry_t *entry = lpr_fd_entry(table, fd);
    lpr_ofd_t *ofd = lpr_fd_ofd_for_entry(table, entry);
    if (ofd == 0) {
        lpr_fd_table_unlock(table);
        return -1;
    }
    ofd->offset = offset;
    (void)lpr_fd_next_generation(table);
    lpr_fd_table_unlock(table);
    return 0;
}

int lpr_fd_table_get_refcount(
    const lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    uint32_t *out_refcount)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    const lpr_fd_entry_t *entry = lpr_fd_entry_const(table, fd);
    const lpr_ofd_t *ofd = lpr_fd_ofd_for_entry_const(table, entry);
    if (ofd == 0 || out_refcount == 0) {
        lpr_fd_table_unlock((lpr_fd_table_t *)table);
        return -1;
    }
    *out_refcount = ofd->refcount;
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return 0;
}

uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    uint32_t count = 0;
    if (lpr_fd_table_valid(table)) {
        for (uint32_t i = 0; i < table->entry_count; i++) {
            count += table->entries[i].active ? 1u : 0u;
        }
    }
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return count;
}

uint32_t lpr_fd_table_live_ofd_count(const lpr_fd_table_t *table)
{
    lpr_fd_table_lock((lpr_fd_table_t *)table);
    uint32_t count = 0;
    if (lpr_fd_table_valid(table)) {
        for (uint32_t i = 0; i < table->ofd_count; i++) {
            count += table->ofds[i].active ? 1u : 0u;
        }
    }
    lpr_fd_table_unlock((lpr_fd_table_t *)table);
    return count;
}
