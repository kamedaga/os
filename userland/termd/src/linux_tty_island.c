#include "linux_tty_island.h"

#include "null_device_backend.h"

#include <kobox/device_pachaos_capsule.h>
#include <kobox/module.h>
#include <kobox/platform.h>
#include <kobox/shim.h>
#include "linux_subsystem/fs/fs.h"
#include "linux_subsystem/fs/kernel_object_registry.h"
#include "linux_subsystem/kvm/kvm_symbols.h"
#include "loader/module_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

static const char *const linux_tty_sources[] = {
    "drivers/tty/tty_io.c",
    "drivers/tty/n_tty.c",
    "drivers/tty/tty_ioctl.c",
    "drivers/tty/tty_ldisc.c",
    "drivers/tty/tty_buffer.c",
    "drivers/tty/tty_port.c",
    "drivers/tty/tty_mutex.c",
    "drivers/tty/tty_ldsem.c",
    "drivers/tty/tty_baudrate.c",
    "drivers/tty/tty_jobctrl.c",
    "drivers/tty/n_null.c",
    "drivers/tty/pty.c",
    "drivers/tty/hvc/hvc_console.c",
    "drivers/char/virtio_console.c",
    "fs/devpts/inode.c",
};

enum {
    TERMD_LINUX_TTYAUX_MAJOR = 5,
    TERMD_LINUX_PTMX_MINOR = 2,
    TERMD_LINUX_HVC_MAJOR = 229,
    TERMD_LINUX_UNIX98_PTY_SLAVE_MAJOR = 136,
    TERMD_LINUX_TTY_HANDLE_MAX = 32,
    TERMD_LINUX_FAKE_INODE_BYTES = 768,
    TERMD_LINUX_FAKE_MAPPING_BYTES = 256,
    TERMD_LINUX_FAKE_FILE_BYTES = 1024,
    TERMD_LINUX_FAKE_PATH_BYTES = 16,
    TERMD_LINUX_FAKE_KIOCB_BYTES = 64,
    TERMD_LINUX_FAKE_IOV_ITER_BYTES = 128,
    TERMD_LINUX_FILE_MODE_OFFSET = 0x14,
    TERMD_LINUX_FILE_FLAGS_OFFSET = 0x48,
    TERMD_LINUX_FILE_PATH_OFFSET = 0x98,
    TERMD_LINUX_FILE_INODE_OFFSET = 0xa8,
    TERMD_LINUX_FILE_OP_OFFSET = 0xb0,
    TERMD_LINUX_FILE_PRIVATE_DATA_OFFSET = 0xc8,
    TERMD_LINUX_KIOCB_FILE_OFFSET = 0x0,
    TERMD_LINUX_KIOCB_POS_OFFSET = 0x8,
    TERMD_LINUX_IOV_ITER_COUNT_OFFSET = 0x18,
    TERMD_LINUX_IOV_ITER_BUFFER_OFFSET = 0x20,
    TERMD_LINUX_IOV_ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    TERMD_LINUX_INODE_MAPPING_OFFSET = 0x30,
    TERMD_LINUX_INODE_RDEV_OFFSET = 0x4c,
    TERMD_LINUX_INODE_CDEV_OFFSET = 0x238,
    TERMD_LINUX_PATH_DENTRY_OFFSET = 0x8,
    TERMD_LINUX_DENTRY_FSDATA_OFFSET = 0x80,
    TERMD_LINUX_TTY_DRIVER_OFFSET = 0x10,
    TERMD_LINUX_TTY_FLAGS_OFFSET = 0x1a0,
    TERMD_LINUX_TTY_COUNT_OFFSET = 0x1a8,
    TERMD_LINUX_TTY_CTRL_PGRP_OFFSET = 0x1c0,
    TERMD_LINUX_TTY_CTRL_SESSION_OFFSET = 0x1c8,
    TERMD_LINUX_TTY_LINK_OFFSET = 0x1e0,
    TERMD_LINUX_TTY_DRIVER_SUBTYPE_OFFSET = 0x3a,
    TERMD_LINUX_TASK_SIGNAL_OFFSET = 0x848,
    TERMD_LINUX_TASK_SIGHAND_OFFSET = 0x850,
    TERMD_LINUX_TASK_BLOCKED_OFFSET = 0x858,
    TERMD_LINUX_TASK_REAL_BLOCKED_OFFSET = 0x860,
    TERMD_LINUX_SIGNAL_PIDS_OFFSET = 0x168,
    TERMD_LINUX_SIGNAL_TTY_OLD_PGRP_OFFSET = 0x190,
    TERMD_LINUX_SIGNAL_LEADER_OFFSET = 0x198,
    TERMD_LINUX_SIGNAL_TTY_OFFSET = 0x1a0,
    TERMD_LINUX_SIGHAND_ACTION_OFFSET = 0x20,
    TERMD_LINUX_K_SIGACTION_BYTES = 0x20,
    TERMD_LINUX_SIGACTION_HANDLER_OFFSET = 0x0,
    TERMD_LINUX_SIGNAL_MAX = 64,
    TERMD_LINUX_SIG_IGN = 1,
    TERMD_LINUX_PIDTYPE_PID = 0,
    TERMD_LINUX_PIDTYPE_TGID = 1,
    TERMD_LINUX_PIDTYPE_PGID = 2,
    TERMD_LINUX_PIDTYPE_SID = 3,
    TERMD_LINUX_FMODE_READ = 0x1,
    TERMD_LINUX_FMODE_WRITE = 0x2,
    TERMD_LINUX_TCGETS = 0x5401,
    TERMD_LINUX_TIOCGWINSZ = 0x5413,
    TERMD_LINUX_TIOCSWINSZ = 0x5414,
    TERMD_LINUX_TIOCGPGRP = 0x540f,
    TERMD_LINUX_TIOCSPGRP = 0x5410,
    TERMD_LINUX_FIONREAD = 0x541b,
    TERMD_LINUX_ESRCH = 3,
    TERMD_LINUX_EPOLLIN = 0x0001,
    TERMD_LINUX_EPOLLPRI = 0x0002,
    TERMD_LINUX_EPOLLOUT = 0x0004,
    TERMD_LINUX_EPOLLERR = 0x0008,
    TERMD_LINUX_EPOLLHUP = 0x0010,
    TERMD_LINUX_EPOLLRDNORM = 0x0040,
    TERMD_LINUX_EPOLLWRNORM = 0x0100,
    TERMD_LINUX_ERESTARTSYS = 512,
    TERMD_LINUX_EINTR = 4,
    TERMD_LINUX_EAGAIN = 11,
};

typedef int (*termd_linux_fops_open_fn)(void *inode, void *file);
typedef int (*termd_linux_fops_release_fn)(void *inode, void *file);
typedef long (*termd_linux_fops_ioctl_fn)(void *file, unsigned int cmd, unsigned long arg);
typedef uint32_t (*termd_linux_fops_poll_fn)(void *file, void *wait);
typedef long (*termd_linux_fops_iter_fn)(void *kiocb, void *iter);

static void termd_write_u16(void *data, size_t offset, uint16_t value);

typedef struct termd_linux_owner_context {
    unsigned long old_gs;
    kb_module_t *previous_owner;
    int active;
} termd_linux_owner_context_t;

typedef struct termd_linux_tty_handle {
    uint8_t active;
    uint8_t master;
    uint16_t reserved0;
    uint32_t flags;
    uint32_t ref_count;
    uint32_t reserved_ref;
    uint64_t handle;
    uint64_t dev;
    uint32_t pts_index;
    uint32_t session_id;
    uint32_t process_id;
    uint32_t pgrp_id;
    uint32_t reserved_owner;
    void *cdev;
    void *fops;
    void *open;
    void *ioctl;
    void *poll;
    void *read_iter;
    void *write_iter;
    void *release;
    kb_module_t *owner;
    uint8_t inode[TERMD_LINUX_FAKE_INODE_BYTES];
    uint8_t mapping[TERMD_LINUX_FAKE_MAPPING_BYTES];
    uint8_t file[TERMD_LINUX_FAKE_FILE_BYTES];
    uint8_t path[TERMD_LINUX_FAKE_PATH_BYTES];
} termd_linux_tty_handle_t;

static termd_linux_tty_handle_t tty_handles[TERMD_LINUX_TTY_HANDLE_MAX];
static uint64_t next_tty_handle = 1;

static void write_ptr_field(void *base, size_t offset, const void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u32_field(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u64_field(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void *read_ptr_field(const void *base, size_t offset)
{
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint64_t read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint32_t read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint16_t read_u16_field(const void *base, size_t offset)
{
    uint16_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void log_tty_state(const char *label, const void *tty)
{
    if (tty == NULL) {
        printf("[termd] linux tty state %s tty=null\n", label);
        return;
    }
    void *driver = read_ptr_field(tty, TERMD_LINUX_TTY_DRIVER_OFFSET);
    void *link = read_ptr_field(tty, TERMD_LINUX_TTY_LINK_OFFSET);
    printf(
        "[termd] linux tty state %s tty=%p driver=%p subtype=%u flags=0x%llx count=%u link=%p\n",
        label,
        tty,
        driver,
        driver != NULL ? (unsigned)read_u16_field(driver, TERMD_LINUX_TTY_DRIVER_SUBTYPE_OFFSET) : 0u,
        (unsigned long long)read_u64_field(tty, TERMD_LINUX_TTY_FLAGS_OFFSET),
        (unsigned)read_u32_field(tty, TERMD_LINUX_TTY_COUNT_OFFSET),
        link);
}

static void *handle_file_tty(const termd_linux_tty_handle_t *handle)
{
    if (handle == NULL) {
        return NULL;
    }
    void *priv = read_ptr_field(handle->file, TERMD_LINUX_FILE_PRIVATE_DATA_OFFSET);
    return priv != NULL ? read_ptr_field(priv, 0) : NULL;
}

static void drain_linux_tty_work(void)
{
    for (unsigned i = 0; i < 4; i++) {
        kb_run_deferred_work();
        if (kb_handle_any_irq(0) != 0) {
            break;
        }
    }
    kb_run_deferred_work();
}

void termd_linux_tty_island_pump(struct termd_linux_tty_island *island)
{
    if (island == NULL || !island->ready) {
        return;
    }
    drain_linux_tty_work();
}

static int enter_owner_context(kb_module_t *owner, termd_linux_owner_context_t *context)
{
    if (context == NULL) {
        return 0;
    }
    memset(context, 0, sizeof(*context));
    if (owner == NULL) {
        return 0;
    }
    context->previous_owner = kb_loader_active_module();
    if (kb_loader_enter_module_context(owner, &context->old_gs) != KB_OK) {
        return 0;
    }
    kb_loader_set_active_module(owner);
    context->active = 1;
    return 1;
}

static kb_module_t *handle_function_owner(
    const termd_linux_tty_handle_t *handle,
    const void *function)
{
    kb_module_t *owner = kb_module_find_owner_for_address(function);
    if (owner != NULL) {
        return owner;
    }
    return handle == NULL ? NULL : handle->owner;
}

static int enter_handle_function_context(
    const termd_linux_tty_handle_t *handle,
    const void *function,
    termd_linux_owner_context_t *context)
{
    return enter_owner_context(handle_function_owner(handle, function), context);
}

static void leave_owner_context(const termd_linux_owner_context_t *context)
{
    if (context == NULL || !context->active) {
        return;
    }
    kb_loader_leave_module_context(context->old_gs);
    kb_loader_set_active_module(context->previous_owner);
}

typedef struct termd_linux_tty_owner_ids {
    uint32_t session_id;
    uint32_t process_id;
    uint32_t pgrp_id;
} termd_linux_tty_owner_ids_t;

static uint32_t termd_linux_tty_wire_id(uint64_t value)
{
    return value != 0 && value <= UINT32_MAX ? (uint32_t)value : 0;
}

static termd_linux_tty_owner_ids_t termd_linux_tty_handle_ids(
    const termd_linux_tty_handle_t *handle)
{
    termd_linux_tty_owner_ids_t ids = {0, 0, 0};
    if (handle != NULL) {
        ids.session_id = handle->session_id;
        ids.process_id = handle->process_id;
        ids.pgrp_id = handle->pgrp_id;
    }
    return ids;
}

static termd_linux_tty_owner_ids_t termd_linux_tty_merge_ids(
    const termd_linux_tty_handle_t *handle,
    uint64_t session_id,
    uint64_t process_id,
    uint64_t pgrp_id)
{
    termd_linux_tty_owner_ids_t ids = termd_linux_tty_handle_ids(handle);
    const uint32_t wire_session = termd_linux_tty_wire_id(session_id);
    const uint32_t wire_process = termd_linux_tty_wire_id(process_id);
    const uint32_t wire_pgrp = termd_linux_tty_wire_id(pgrp_id);
    if (wire_session != 0) {
        ids.session_id = wire_session;
    }
    if (wire_process != 0) {
        ids.process_id = wire_process;
    }
    if (wire_pgrp != 0) {
        ids.pgrp_id = wire_pgrp;
    }
    if (ids.process_id == 0) {
        ids.process_id = ids.pgrp_id != 0 ? ids.pgrp_id : ids.session_id;
    }
    if (ids.pgrp_id == 0) {
        ids.pgrp_id = ids.process_id != 0 ? ids.process_id : ids.session_id;
    }
    if (ids.session_id == 0) {
        ids.session_id = ids.process_id != 0 ? ids.process_id : ids.pgrp_id;
    }
    return ids;
}

static void termd_linux_tty_record_owner(
    termd_linux_tty_handle_t *handle,
    const termd_wire_open_t *request)
{
    if (handle == NULL || request == NULL) {
        return;
    }
    const termd_linux_tty_owner_ids_t ids = termd_linux_tty_merge_ids(
        NULL,
        request->session_id,
        request->process_id,
        request->pgrp_id);
    handle->session_id = ids.session_id;
    handle->process_id = ids.process_id;
    handle->pgrp_id = ids.pgrp_id;
}

static void *termd_linux_tty_real_tty(const termd_linux_tty_handle_t *handle)
{
    void *tty = handle_file_tty(handle);
    if (tty == NULL) {
        return NULL;
    }
    if (handle != NULL && handle->master) {
        void *link = read_ptr_field(tty, TERMD_LINUX_TTY_LINK_OFFSET);
        if (link != NULL) {
            return link;
        }
    }
    return tty;
}

static void termd_linux_signal_write_pid(void *signal, uint32_t pid_type, void *pid)
{
    if (signal != NULL && pid_type < TERMD_LINUX_PIDTYPE_SID + 1u && pid != NULL) {
        write_ptr_field(
            signal,
            TERMD_LINUX_SIGNAL_PIDS_OFFSET + ((size_t)pid_type * sizeof(void *)),
            pid);
    }
}

static uint64_t termd_linux_signal_bit(uint32_t sig)
{
    if (sig == 0 || sig > TERMD_LINUX_SIGNAL_MAX) {
        return 0;
    }
    return 1ull << (sig - 1u);
}

static void termd_linux_tty_write_task_signal_state(
    void *task,
    uint64_t signal_mask,
    uint64_t signal_ignored)
{
    if (task == NULL) {
        return;
    }
    write_u64_field(task, TERMD_LINUX_TASK_BLOCKED_OFFSET, signal_mask);
    write_u64_field(task, TERMD_LINUX_TASK_REAL_BLOCKED_OFFSET, signal_mask);

    void *sighand = read_ptr_field(task, TERMD_LINUX_TASK_SIGHAND_OFFSET);
    if (sighand == NULL) {
        return;
    }
    for (uint32_t sig = 1; sig <= TERMD_LINUX_SIGNAL_MAX; sig += 1) {
        const uint64_t bit = termd_linux_signal_bit(sig);
        void *handler = (signal_ignored & bit) != 0 ?
            (void *)(uintptr_t)TERMD_LINUX_SIG_IGN :
            NULL;
        write_ptr_field(
            sighand,
            TERMD_LINUX_SIGHAND_ACTION_OFFSET +
                ((size_t)(sig - 1u) * TERMD_LINUX_K_SIGACTION_BYTES) +
                TERMD_LINUX_SIGACTION_HANDLER_OFFSET,
            handler);
    }
}

static void termd_linux_tty_write_task_state(
    void *task,
    const termd_linux_tty_owner_ids_t *ids,
    void *process,
    void *pgrp,
    void *session,
    void *tty,
    uint64_t signal_mask,
    uint64_t signal_ignored)
{
    if (task == NULL || ids == NULL) {
        return;
    }
    termd_linux_tty_write_task_signal_state(task, signal_mask, signal_ignored);
    void *signal = read_ptr_field(task, TERMD_LINUX_TASK_SIGNAL_OFFSET);
    if (signal == NULL) {
        return;
    }
    termd_linux_signal_write_pid(signal, TERMD_LINUX_PIDTYPE_PID, process);
    termd_linux_signal_write_pid(signal, TERMD_LINUX_PIDTYPE_TGID, process);
    termd_linux_signal_write_pid(signal, TERMD_LINUX_PIDTYPE_PGID, pgrp);
    termd_linux_signal_write_pid(signal, TERMD_LINUX_PIDTYPE_SID, session);
    write_u32_field(
        signal,
        TERMD_LINUX_SIGNAL_LEADER_OFFSET,
        ids->process_id != 0 && ids->process_id == ids->session_id ? 1u : 0u);
    write_ptr_field(signal, TERMD_LINUX_SIGNAL_TTY_OLD_PGRP_OFFSET, NULL);
    if (tty != NULL) {
        write_ptr_field(signal, TERMD_LINUX_SIGNAL_TTY_OFFSET, tty);
    }
}

static void termd_linux_tty_sync_current_state(
    termd_linux_tty_handle_t *handle,
    termd_linux_tty_owner_ids_t ids,
    int install_initial_foreground,
    const void *function,
    uint64_t signal_mask,
    uint64_t signal_ignored)
{
    if (handle == NULL || ids.session_id == 0 || ids.pgrp_id == 0) {
        return;
    }
    void *real_tty = termd_linux_tty_real_tty(handle);
    termd_linux_owner_context_t context;
    const void *owner_function = function != NULL ?
        function :
        (handle->ioctl != NULL ? handle->ioctl : handle->open);
    int has_context = enter_handle_function_context(
        handle,
        owner_function,
        &context);
    void *process = ids.process_id != 0 ? kb_find_vpid((int)ids.process_id) : NULL;
    void *pgrp = kb_find_vpid((int)ids.pgrp_id);
    void *session = kb_find_vpid((int)ids.session_id);
    if (process == NULL) {
        process = pgrp != NULL ? pgrp : session;
    }
    void *current_task = kb_loader_module_current_task(kb_loader_active_module());
    termd_linux_tty_write_task_state(
        current_task,
        &ids,
        process,
        pgrp,
        session,
        real_tty,
        signal_mask,
        signal_ignored);
    termd_linux_tty_write_task_state(
        kb_pid_task(process, TERMD_LINUX_PIDTYPE_PID),
        &ids,
        process,
        pgrp,
        session,
        real_tty,
        signal_mask,
        signal_ignored);
    termd_linux_tty_write_task_state(
        kb_pid_task(pgrp, TERMD_LINUX_PIDTYPE_PGID),
        &ids,
        process,
        pgrp,
        session,
        real_tty,
        signal_mask,
        signal_ignored);
    termd_linux_tty_write_task_state(
        kb_pid_task(session, TERMD_LINUX_PIDTYPE_SID),
        &ids,
        process,
        pgrp,
        session,
        real_tty,
        signal_mask,
        signal_ignored);
    if (real_tty != NULL && session != NULL) {
        write_ptr_field(real_tty, TERMD_LINUX_TTY_CTRL_SESSION_OFFSET, session);
        if (install_initial_foreground &&
            pgrp != NULL &&
            read_ptr_field(real_tty, TERMD_LINUX_TTY_CTRL_PGRP_OFFSET) == NULL)
        {
            write_ptr_field(real_tty, TERMD_LINUX_TTY_CTRL_PGRP_OFFSET, pgrp);
        }
    }
    if (has_context) {
        leave_owner_context(&context);
    }
}

int termd_linux_tty_island_refresh_ptmx(struct termd_linux_tty_island *island)
{
    if (island == NULL) {
        return -22;
    }
    island->ptmx_registered = 0;
    island->ptmx_cdev = NULL;
    island->ptmx_fops = NULL;
    island->ptmx_open = NULL;
    island->ptmx_ioctl = NULL;
    island->ptmx_poll = NULL;
    island->ptmx_read_iter = NULL;
    island->ptmx_write_iter = NULL;
    island->ptmx_release = NULL;
    island->ptmx_owner = NULL;
    island->ptmx_dev = kb_linux_kernel_encode_dev(
        TERMD_LINUX_TTYAUX_MAJOR,
        TERMD_LINUX_PTMX_MINOR);

    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(island->ptmx_dev);
    if (record == NULL || !record->has_fops_view || record->fops_view.open == NULL) {
        return -19;
    }

    island->ptmx_registered = 1;
    island->ptmx_cdev = record->cdev;
    island->ptmx_fops = record->fops;
    island->ptmx_open = record->fops_view.open;
    island->ptmx_ioctl = record->fops_view.unlocked_ioctl;
    island->ptmx_poll = record->fops_view.poll;
    island->ptmx_read_iter = record->fops_view.read_iter;
    island->ptmx_write_iter = record->fops_view.write_iter;
    island->ptmx_release = record->fops_view.release;
    island->ptmx_owner = record->owner_module;
    return 0;
}

static void log_hvc0_cdev_state(void)
{
    const uint64_t dev = kb_linux_kernel_encode_dev(TERMD_LINUX_HVC_MAJOR, 0);
    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(dev);
    if (record == NULL || !record->has_fops_view) {
        printf("[termd] linux tty hvc0 cdev missing dev=%u:0\n", (unsigned)TERMD_LINUX_HVC_MAJOR);
        return;
    }
    printf(
        "[termd] linux tty hvc0 cdev ready dev=%u:0 cdev=%p fops=%p owner=%p open=%p open_gs=0x%lx read_iter=%p write_iter=%p\n",
        (unsigned)TERMD_LINUX_HVC_MAJOR,
        record->cdev,
        record->fops,
        (void *)record->owner_module,
        record->fops_view.open,
        kb_module_kernel_gs_for_address(record->fops_view.open),
        record->fops_view.read_iter,
        record->fops_view.write_iter);
}

static int hvc0_cdev_ready(void)
{
    const uint64_t dev = kb_linux_kernel_encode_dev(TERMD_LINUX_HVC_MAJOR, 0);
    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(dev);
    return record != NULL && record->has_fops_view && record->fops_view.open != NULL;
}

static int wait_hvc0_cdev_ready(void)
{
    enum {
        HVC0_READY_POLLS = 256,
        HVC0_READY_POLL_NS = 1000000,
    };

    for (unsigned i = 0; i < HVC0_READY_POLLS; i++) {
        if (hvc0_cdev_ready()) {
            if (i != 0) {
                printf("[termd] linux tty hvc0 cdev ready after irq/work polls=%u\n", i);
            }
            return 0;
        }
        kb_run_deferred_work();
        const int irq_status = kb_handle_any_irq(HVC0_READY_POLL_NS);
        if (i < 8 || irq_status == 0) {
            printf("[termd] linux tty hvc0 wait poll=%u irq_status=%d\n", i, irq_status);
        }
    }
    return hvc0_cdev_ready() ? 0 : -110;
}

static void pump_hvc0_cdev_ready(void)
{
    enum {
        HVC0_OPEN_POLLS = 64,
        HVC0_OPEN_POLL_NS = 1000000,
    };

    for (unsigned i = 0; i < HVC0_OPEN_POLLS && !hvc0_cdev_ready(); i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(HVC0_OPEN_POLL_NS);
    }
}


static termd_linux_tty_handle_t *find_handle(uint64_t handle)
{
    if (handle == 0) {
        return NULL;
    }
    for (size_t i = 0; i < TERMD_LINUX_TTY_HANDLE_MAX; i++) {
        if (tty_handles[i].active && tty_handles[i].handle == handle) {
            return &tty_handles[i];
        }
    }
    return NULL;
}

static termd_linux_tty_handle_t *alloc_handle(void)
{
    for (size_t i = 0; i < TERMD_LINUX_TTY_HANDLE_MAX; i++) {
        if (!tty_handles[i].active) {
            memset(&tty_handles[i], 0, sizeof(tty_handles[i]));
            tty_handles[i].active = 1;
            tty_handles[i].handle = next_tty_handle++;
            if (next_tty_handle == 0) {
                next_tty_handle = 1;
            }
            return &tty_handles[i];
        }
    }
    return NULL;
}

static void prepare_tty_file(termd_linux_tty_handle_t *handle, uint64_t flags)
{
    memset(handle->inode, 0, sizeof(handle->inode));
    memset(handle->mapping, 0, sizeof(handle->mapping));
    memset(handle->file, 0, sizeof(handle->file));
    memset(handle->path, 0, sizeof(handle->path));

    if (handle->master) {
        (void)kb_fs_subsystem_path_pts(handle->path);
    } else if (kb_linux_kernel_decode_major(handle->dev) == TERMD_LINUX_UNIX98_PTY_SLAVE_MAJOR) {
        (void)kb_fs_subsystem_path_devpts_index(handle->path, handle->pts_index);
    }
    write_ptr_field(handle->inode, TERMD_LINUX_INODE_MAPPING_OFFSET, handle->mapping);
    write_u64_field(handle->inode, TERMD_LINUX_INODE_RDEV_OFFSET, handle->dev);
    write_ptr_field(handle->inode, TERMD_LINUX_INODE_CDEV_OFFSET, handle->cdev);
    memcpy(handle->file + TERMD_LINUX_FILE_PATH_OFFSET, handle->path, TERMD_LINUX_FAKE_PATH_BYTES);
    write_ptr_field(handle->file, TERMD_LINUX_FILE_INODE_OFFSET, handle->inode);
    write_ptr_field(handle->file, TERMD_LINUX_FILE_OP_OFFSET, handle->fops);
    write_u32_field(handle->file, TERMD_LINUX_FILE_MODE_OFFSET, TERMD_LINUX_FMODE_READ | TERMD_LINUX_FMODE_WRITE);
    write_u32_field(handle->file, TERMD_LINUX_FILE_FLAGS_OFFSET, (uint32_t)flags);
    write_ptr_field(handle->file, TERMD_LINUX_FILE_PRIVATE_DATA_OFFSET, NULL);
}

static int call_handle_open(termd_linux_tty_handle_t *handle)
{
    if (handle == NULL || handle->open == NULL) {
        return -19;
    }
    termd_linux_owner_context_t context;
    int has_context = enter_handle_function_context(handle, handle->open, &context);
    const int result = ((termd_linux_fops_open_fn)handle->open)(handle->inode, handle->file);
    if (has_context) {
        leave_owner_context(&context);
    }
    return result;
}

static long call_handle_ioctl(
    termd_linux_tty_handle_t *handle,
    unsigned int request,
    void *arg)
{
    if (handle == NULL || handle->ioctl == NULL) {
        return -19;
    }
    termd_linux_owner_context_t context;
    int has_context = enter_handle_function_context(handle, handle->ioctl, &context);
    drain_linux_tty_work();
    if (request == 0x40045431u) {
        uint32_t value = 0;
        if (arg != NULL) {
            memcpy(&value, arg, sizeof(value));
        }
        void *tty = handle_file_tty(handle);
        printf(
            "[termd] linux tty ioctl call handle=%llu request=0x%x arg=%p value=%u ioctl=%p tty=%p flags_before=0x%llx\n",
            (unsigned long long)handle->handle,
            request,
            arg,
            value,
            handle->ioctl,
            tty,
            tty != NULL ? (unsigned long long)read_u64_field(tty, TERMD_LINUX_TTY_FLAGS_OFFSET) : 0ull);
    }
    const long result = ((termd_linux_fops_ioctl_fn)handle->ioctl)(
        handle->file,
        request,
        (unsigned long)(uintptr_t)arg);
    drain_linux_tty_work();
    if (request == 0x40045431u) {
        void *tty = handle_file_tty(handle);
        printf(
            "[termd] linux tty ioctl done handle=%llu request=0x%x result=%ld tty=%p flags_after=0x%llx\n",
            (unsigned long long)handle->handle,
            request,
            result,
            tty,
            tty != NULL ? (unsigned long long)read_u64_field(tty, TERMD_LINUX_TTY_FLAGS_OFFSET) : 0ull);
    }
    if (has_context) {
        leave_owner_context(&context);
    }
    return result;
}

static void initialize_ptmx_winsize(struct termd_linux_tty_island *island, termd_linux_tty_handle_t *handle)
{
    (void)island;
    uint8_t winsize[8];
    memset(winsize, 0, sizeof(winsize));
    termd_write_u16(winsize, 0, 24);
    termd_write_u16(winsize, 2, 80);
    const long result = call_handle_ioctl(handle, TERMD_LINUX_TIOCSWINSZ, winsize);
    if (result != 0) {
        printf("[termd] linux tty ptmx initial winsize failed status=%ld\n", result);
    }
}

int termd_linux_tty_island_open_ptmx(
    struct termd_linux_tty_island *island,
    uint64_t flags,
    uint64_t *out_handle)
{
    if (island == NULL || out_handle == NULL) {
        return -22;
    }
    *out_handle = 0;
    if (!island->ready || !island->ptmx_registered || island->ptmx_open == NULL) {
        return -19;
    }
    termd_linux_tty_handle_t *handle = alloc_handle();
    if (handle == NULL) {
        return -24;
    }

    handle->master = 1;
    handle->dev = island->ptmx_dev;
    handle->cdev = island->ptmx_cdev;
    handle->fops = island->ptmx_fops;
    handle->open = island->ptmx_open;
    handle->ioctl = island->ptmx_ioctl;
    handle->poll = island->ptmx_poll;
    handle->read_iter = island->ptmx_read_iter;
    handle->write_iter = island->ptmx_write_iter;
    handle->release = island->ptmx_release;
    handle->owner = island->ptmx_owner;
    prepare_tty_file(handle, flags);
    int result = call_handle_open(handle);
    if (result != 0) {
        printf("[termd] linux tty ptmx open failed status=%d\n", result);
        memset(handle, 0, sizeof(*handle));
        return result;
    }

    handle->flags = (uint32_t)flags;
    handle->ref_count = 1;
    initialize_ptmx_winsize(island, handle);
    *out_handle = handle->handle;
    printf("[termd] linux tty ptmx open ready handle=%llu\n", (unsigned long long)handle->handle);
    return 0;
}

int termd_linux_tty_island_open_pts(
    struct termd_linux_tty_island *island,
    const termd_wire_open_t *request,
    uint64_t *out_handle)
{
    if (island == NULL || request == NULL || out_handle == NULL) {
        return -22;
    }
    *out_handle = 0;
    if (!island->ready) {
        return -19;
    }
    if (request->pts_index >= TERMD_LINUX_TTY_HANDLE_MAX) {
        return -2;
    }

    const uint64_t dev = kb_linux_kernel_encode_dev(
        TERMD_LINUX_UNIX98_PTY_SLAVE_MAJOR,
        (unsigned)request->pts_index);
    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(dev);
    if (record == NULL || !record->has_fops_view || record->fops_view.open == NULL) {
        return -19;
    }

    uint8_t path_probe[TERMD_LINUX_FAKE_PATH_BYTES];
    if (kb_fs_subsystem_path_devpts_index(path_probe, (unsigned)request->pts_index) != 0) {
        return -2;
    }
    void *probe_dentry = read_ptr_field(path_probe, TERMD_LINUX_PATH_DENTRY_OFFSET);
    void *probe_tty = probe_dentry != NULL ?
        read_ptr_field(probe_dentry, TERMD_LINUX_DENTRY_FSDATA_OFFSET) : NULL;
    void *probe_link = probe_tty != NULL ?
        read_ptr_field(probe_tty, TERMD_LINUX_TTY_LINK_OFFSET) : NULL;
    printf(
        "[termd] linux tty pts open probe index=%llu dentry=%p tty=%p link=%p\n",
        (unsigned long long)request->pts_index,
        probe_dentry,
        probe_tty,
        probe_link);
    log_tty_state("pts-probe", probe_tty);
    log_tty_state("pts-probe-link", probe_link);

    termd_linux_tty_handle_t *handle = alloc_handle();
    if (handle == NULL) {
        return -24;
    }
    handle->master = 0;
    handle->pts_index = (uint32_t)request->pts_index;
    handle->dev = dev;
    handle->cdev = record->cdev;
    handle->fops = record->fops;
    handle->open = record->fops_view.open;
    handle->ioctl = record->fops_view.unlocked_ioctl;
    handle->poll = record->fops_view.poll;
    handle->read_iter = record->fops_view.read_iter;
    handle->write_iter = record->fops_view.write_iter;
    handle->release = record->fops_view.release;
    handle->owner = record->owner_module;
    prepare_tty_file(handle, request->flags);
    termd_linux_tty_record_owner(handle, request);
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_handle_ids(handle),
        0,
        handle->open,
        request->signal_mask,
        request->signal_ignored);

    int result = call_handle_open(handle);
    if (result != 0) {
        printf(
            "[termd] linux tty pts open failed index=%llu status=%d\n",
            (unsigned long long)request->pts_index,
            result);
        log_tty_state("pts-failed", probe_tty);
        log_tty_state("pts-failed-link", probe_link);
        memset(handle, 0, sizeof(*handle));
        return result;
    }

    handle->flags = (uint32_t)request->flags;
    handle->ref_count = 1;
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_handle_ids(handle),
        1,
        handle->open,
        request->signal_mask,
        request->signal_ignored);
    *out_handle = handle->handle;
    printf(
        "[termd] linux tty pts open ready index=%llu handle=%llu\n",
        (unsigned long long)request->pts_index,
        (unsigned long long)handle->handle);
    return 0;
}

int termd_linux_tty_island_open_hvc(
    struct termd_linux_tty_island *island,
    const termd_wire_open_t *request,
    uint64_t *out_handle)
{
    if (island == NULL || request == NULL || out_handle == NULL) {
        return -22;
    }
    *out_handle = 0;
    if (!island->ready) {
        return -19;
    }
    if (request->pts_index >= TERMD_LINUX_TTY_HANDLE_MAX) {
        return -2;
    }

    const uint64_t dev = kb_linux_kernel_encode_dev(
        TERMD_LINUX_HVC_MAJOR,
        (unsigned)request->pts_index);
    pump_hvc0_cdev_ready();
    const kb_cdev_record_t *record = kb_linux_kernel_find_active_cdev(dev);
    if (record == NULL || !record->has_fops_view || record->fops_view.open == NULL) {
        printf(
            "[termd] linux tty hvc open missing index=%llu dev=%u:%llu\n",
            (unsigned long long)request->pts_index,
            (unsigned)TERMD_LINUX_HVC_MAJOR,
            (unsigned long long)request->pts_index);
        return -19;
    }

    termd_linux_tty_handle_t *handle = alloc_handle();
    if (handle == NULL) {
        return -24;
    }
    handle->master = 0;
    handle->pts_index = (uint32_t)request->pts_index;
    handle->dev = dev;
    handle->cdev = record->cdev;
    handle->fops = record->fops;
    handle->open = record->fops_view.open;
    handle->ioctl = record->fops_view.unlocked_ioctl;
    handle->poll = record->fops_view.poll;
    handle->read_iter = record->fops_view.read_iter;
    handle->write_iter = record->fops_view.write_iter;
    handle->release = record->fops_view.release;
    handle->owner = record->owner_module;
    prepare_tty_file(handle, request->flags);
    termd_linux_tty_record_owner(handle, request);
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_handle_ids(handle),
        0,
        handle->open,
        request->signal_mask,
        request->signal_ignored);

    int result = call_handle_open(handle);
    drain_linux_tty_work();
    if (result != 0) {
        printf(
            "[termd] linux tty hvc open failed index=%llu status=%d\n",
            (unsigned long long)request->pts_index,
            result);
        memset(handle, 0, sizeof(*handle));
        return result;
    }

    handle->flags = (uint32_t)request->flags;
    handle->ref_count = 1;
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_handle_ids(handle),
        1,
        handle->open,
        request->signal_mask,
        request->signal_ignored);
    *out_handle = handle->handle;
    printf(
        "[termd] linux tty hvc open ready index=%llu handle=%llu\n",
        (unsigned long long)request->pts_index,
        (unsigned long long)handle->handle);
    return 0;
}

int termd_linux_tty_island_open_ctty(
    struct termd_linux_tty_island *island,
    const termd_wire_open_t *request,
    uint64_t *out_handle)
{
    if (request == NULL) {
        return -22;
    }
    termd_wire_open_t hvc_request = *request;
    hvc_request.pts_index = 0;
    return termd_linux_tty_island_open_hvc(island, &hvc_request, out_handle);
}

int termd_linux_tty_island_take_signal(
    struct termd_linux_tty_island *island,
    termd_wire_signal_t *request,
    uint64_t *out_result)
{
    if (island == NULL || request == NULL || out_result == NULL) {
        return -22;
    }
    *out_result = 0;
    if (!island->ready) {
        return -19;
    }
    drain_linux_tty_work();
    int pgrp = 0;
    int sig = 0;
    uint64_t generation = island->signal_generation;
    const int status = kb_take_pending_pgrp_signal(
        island->signal_generation,
        &pgrp,
        &sig,
        &generation);
    if (status < 0) {
        return status;
    }
    if (status == 0 || sig == 0 || pgrp <= 0) {
        request->signo = 0;
        request->pgrp_id = 0;
        request->generation = island->signal_generation;
        return 0;
    }
    island->signal_generation = generation;
    request->signo = (uint32_t)sig;
    request->pgrp_id = (uint32_t)pgrp;
    request->generation = generation;
    *out_result = 1;
    return 0;
}

int termd_linux_tty_island_close(struct termd_linux_tty_island *island, uint64_t handle_id)
{
    if (island == NULL) {
        return -22;
    }
    termd_linux_tty_handle_t *handle = find_handle(handle_id);
    if (handle == NULL) {
        return -9;
    }
    if (handle->ref_count > 1) {
        handle->ref_count--;
        return 0;
    }
    int result = 0;
    if (handle->release != NULL) {
        termd_linux_owner_context_t context;
        int has_context = enter_handle_function_context(handle, handle->release, &context);
        result = ((termd_linux_fops_release_fn)handle->release)(handle->inode, handle->file);
        if (has_context) {
            leave_owner_context(&context);
        }
    }
    memset(handle, 0, sizeof(*handle));
    return result;
}

int termd_linux_tty_island_dup(
    struct termd_linux_tty_island *island,
    uint64_t handle_id,
    uint64_t *out_handle)
{
    if (island == NULL || out_handle == NULL) {
        return -22;
    }
    *out_handle = 0;
    termd_linux_tty_handle_t *handle = find_handle(handle_id);
    if (handle == NULL) {
        return -9;
    }
    if (handle->ref_count == UINT32_MAX) {
        return -24;
    }
    handle->ref_count++;
    *out_handle = handle->handle;
    return 0;
}

static uint16_t termd_read_u16(const void *data, size_t offset)
{
    uint16_t value = 0;
    memcpy(&value, (const uint8_t *)data + offset, sizeof(value));
    return value;
}

static uint32_t termd_read_u32(const void *data, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)data + offset, sizeof(value));
    return value;
}

static void termd_write_u16(void *data, size_t offset, uint16_t value)
{
    memcpy((uint8_t *)data + offset, &value, sizeof(value));
}

int termd_linux_tty_island_ioctl(
    struct termd_linux_tty_island *island,
    termd_wire_ioctl_t *request)
{
    if (island == NULL || request == NULL) {
        return -22;
    }
    if (!island->ready) {
        return -19;
    }
    termd_linux_tty_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) {
        return -9;
    }
    termd_linux_tty_owner_ids_t caller_ids = termd_linux_tty_merge_ids(
        handle,
        request->session_id,
        request->process_id,
        request->pgrp_id);
    if (request->request == TERMD_LINUX_TIOCSPGRP && request->arg0 > 0 && request->arg0 <= UINT32_MAX) {
        termd_linux_tty_owner_ids_t target_ids = caller_ids;
        target_ids.process_id = (uint32_t)request->arg0;
        target_ids.pgrp_id = (uint32_t)request->arg0;
        termd_linux_tty_sync_current_state(
            handle,
            target_ids,
            0,
            handle->ioctl,
            request->signal_mask,
            request->signal_ignored);
    }
    termd_linux_tty_sync_current_state(
        handle,
        caller_ids,
        0,
        handle->ioctl,
        request->signal_mask,
        request->signal_ignored);

    if (request->request == TERMD_LINUX_TIOCSWINSZ) {
        memset(request->data, 0, 8u);
        termd_write_u16(request->data, 0, (uint16_t)request->arg0);
        termd_write_u16(request->data, 2, (uint16_t)request->arg1);
    } else if (request->request == TERMD_LINUX_TIOCSPGRP) {
        uint32_t pgrp = (uint32_t)request->arg0;
        memcpy(request->data, &pgrp, sizeof(pgrp));
    }

    const long result = call_handle_ioctl(
        handle,
        (unsigned int)request->request,
        request->data);

    if (result != 0) {
        printf(
            "[termd] linux tty ioctl failed handle=%llu request=0x%llx status=%ld\n",
            (unsigned long long)request->handle,
            (unsigned long long)request->request,
            result);
        return (int)result;
    }

    if (request->request == TERMD_LINUX_TIOCGWINSZ) {
        request->result0 = termd_read_u16(request->data, 0);
        request->result1 = termd_read_u16(request->data, 2);
    } else if (request->request == TERMD_LINUX_TIOCGPGRP ||
        request->request == TERMD_LINUX_FIONREAD)
    {
        request->result0 = termd_read_u32(request->data, 0);
    }
    return 0;
}

static uint32_t termd_linux_poll_mask_to_wire(uint32_t mask)
{
    uint32_t revents = 0;
    if ((mask & (TERMD_LINUX_EPOLLIN | TERMD_LINUX_EPOLLRDNORM | TERMD_LINUX_EPOLLPRI)) != 0) {
        revents |= TERMD_WIRE_POLLIN;
    }
    if ((mask & (TERMD_LINUX_EPOLLOUT | TERMD_LINUX_EPOLLWRNORM)) != 0) {
        revents |= TERMD_WIRE_POLLOUT;
    }
    if ((mask & TERMD_LINUX_EPOLLERR) != 0) {
        revents |= TERMD_WIRE_POLLERR;
    }
    if ((mask & TERMD_LINUX_EPOLLHUP) != 0) {
        revents |= TERMD_WIRE_POLLHUP;
    }
    return revents;
}

int termd_linux_tty_island_poll(
    struct termd_linux_tty_island *island,
    termd_wire_poll_t *request)
{
    if (island == NULL || request == NULL) {
        return -22;
    }
    if (!island->ready) {
        return -19;
    }
    termd_linux_tty_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) {
        return -9;
    }
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_merge_ids(
            handle,
            request->session_id,
            request->process_id,
            request->pgrp_id),
        0,
        handle->poll,
        request->signal_mask,
        request->signal_ignored);
    if (handle->poll == NULL) {
        request->revents = TERMD_WIRE_POLLERR;
        return 0;
    }

    termd_linux_owner_context_t context;
    int has_context = enter_handle_function_context(handle, handle->poll, &context);
    drain_linux_tty_work();
    const uint32_t mask = ((termd_linux_fops_poll_fn)handle->poll)(handle->file, NULL);
    if (has_context) {
        leave_owner_context(&context);
    }
    request->revents = termd_linux_poll_mask_to_wire(mask) & (
        request->events |
        TERMD_WIRE_POLLERR |
        TERMD_WIRE_POLLHUP);
    return 0;
}

int termd_linux_tty_island_io(
    struct termd_linux_tty_island *island,
    int write,
    termd_wire_io_t *request,
    uint64_t *out_result)
{
    if (island == NULL || request == NULL || out_result == NULL) {
        return -22;
    }
    *out_result = 0;
    if (!island->ready) {
        return -19;
    }
    termd_linux_tty_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) {
        return -9;
    }
    void *iter_fn = write ? handle->write_iter : handle->read_iter;
    if (iter_fn == NULL) {
        return -95;
    }
    termd_linux_tty_sync_current_state(
        handle,
        termd_linux_tty_merge_ids(
            handle,
            request->session_id,
            request->process_id,
            request->pgrp_id),
        0,
        iter_fn,
        request->signal_mask,
        request->signal_ignored);
    if (request->length > TERMD_WIRE_IO_BYTES) {
        request->length = TERMD_WIRE_IO_BYTES;
    }
    if (request->length == 0) {
        return 0;
    }

    uint8_t kiocb[TERMD_LINUX_FAKE_KIOCB_BYTES];
    uint8_t iter[TERMD_LINUX_FAKE_IOV_ITER_BYTES];
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    write_ptr_field(kiocb, TERMD_LINUX_KIOCB_FILE_OFFSET, handle->file);
    write_u64_field(kiocb, TERMD_LINUX_KIOCB_POS_OFFSET, 0);
    write_u64_field(iter, TERMD_LINUX_IOV_ITER_COUNT_OFFSET, request->length);
    write_ptr_field(iter, TERMD_LINUX_IOV_ITER_BUFFER_OFFSET, request->data);
    write_u64_field(iter, TERMD_LINUX_IOV_ITER_BUFFER_CAPACITY_OFFSET, request->length);

    termd_linux_owner_context_t context;
    int has_context = enter_handle_function_context(handle, iter_fn, &context);
    if (!write) {
        drain_linux_tty_work();
    }
    const long result = ((termd_linux_fops_iter_fn)iter_fn)(kiocb, iter);
    drain_linux_tty_work();
    if (has_context) {
        leave_owner_context(&context);
    }
    const int hvc_no_input = !write &&
        result == 0 &&
        kb_linux_kernel_decode_major(handle->dev) == TERMD_LINUX_HVC_MAJOR;
    if (result == -TERMD_LINUX_ERESTARTSYS) {
        termd_linux_owner_context_t clear_context;
        int clear_has_context = enter_handle_function_context(handle, iter_fn, &clear_context);
        kb_clear_current_signal_pending();
        if (clear_has_context) {
            leave_owner_context(&clear_context);
        }
        return -TERMD_LINUX_EINTR;
    }
    if (hvc_no_input) {
        return -TERMD_LINUX_EAGAIN;
    }
    if (result < 0) {
        return (int)result;
    }
    *out_result = (uint64_t)result;
    return 0;
}

static int low_or_error_pointer(const void *ptr)
{
    const uintptr_t value = (uintptr_t)ptr;
    return ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static int termd_linux_tty_island_mount_devpts(struct termd_linux_tty_island *island)
{
    if (island == NULL) {
        return -22;
    }
    island->devpts_root = kb_fs_subsystem_mount_registered("devpts", 0, "devpts", NULL);
    if (low_or_error_pointer(island->devpts_root)) {
        island->devpts_status = (int32_t)(intptr_t)island->devpts_root;
        if (island->devpts_status == 0) {
            island->devpts_status = -19;
        }
        printf("[termd] linux tty devpts mount failed status=%d root=%p\n",
            (int)island->devpts_status,
            island->devpts_root);
        return island->devpts_status;
    }
    island->devpts_status = 0;
    printf("[termd] linux tty devpts mounted root=%p\n", island->devpts_root);
    return 0;
}

static kb_status_t termd_linux_tty_island_create_backend(
    const struct termd_boot_config *cfg,
    kb_device_backend_t **out_backend)
{
    if (cfg == NULL || out_backend == NULL) {
        return KB_ERR_INVALID;
    }
    *out_backend = NULL;
    if (cfg->device_fd >= 16) {
        (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
        (void)setenv("KOBOX_PCI_LAYOUT", "arch68", 1);
        (void)setenv("KOBOX_VIRTIO_NO_INDIRECT", "1", 1);
        (void)setenv("KOBOX_VIRTIO_NO_EVENT_IDX", "1", 1);
        kb_status_t status = kb_pachaos_capsule_device_create(cfg->device_fd, out_backend);
        if (status != KB_OK || *out_backend == NULL) {
            fprintf(stderr,
                "[termd] virtio-console backend create failed fd=%llu status=%d\n",
                (unsigned long long)cfg->device_fd,
                (int)status);
            return status;
        }
        kb_shim_set_device_backend(*out_backend);
        const int dma_status = kb_kvm_prepare_dma_arena(*out_backend);
        if (dma_status != 0) {
            fprintf(stderr,
                "[termd] virtio-console dma arena prepare failed fd=%llu status=%d\n",
                (unsigned long long)cfg->device_fd,
                dma_status);
            return KB_ERR_IO;
        }
        printf("[termd] virtio-console backend ready fd=%llu\n",
            (unsigned long long)cfg->device_fd);
        return KB_OK;
    }
    kb_status_t status = termd_null_device_backend_create(out_backend);
    if (status == KB_OK && *out_backend != NULL) {
        printf("[termd] tty-only null backend ready\n");
    }
    return status;
}

int termd_linux_tty_island_init(
    struct termd_linux_tty_island *island,
    const struct termd_boot_config *cfg)
{
    if (island == NULL || cfg == NULL) {
        return -22;
    }
    memset(island, 0, sizeof(*island));
    island->source_count = (uint32_t)(sizeof(linux_tty_sources) / sizeof(linux_tty_sources[0]));
    island->loader_version = kb_module_loader_version();
    island->configured_module_count = cfg->module_count > TERMD_MAX_MODULES ?
        TERMD_MAX_MODULES :
        (uint32_t)cfg->module_count;

    kb_device_backend_t *backend = NULL;
    kb_status_t status = termd_linux_tty_island_create_backend(cfg, &backend);
    if (status != KB_OK || backend == NULL) {
        island->load_status = (int32_t)status;
        return 0;
    }
    island->backend = backend;

    const kb_platform_desc_t desc = {
        .name = "termd-linux-tty-island",
        .device_backend = backend,
        .interfaces = NULL,
        .interface_count = 0,
    };
    kb_platform_t *platform = NULL;
    status = kb_platform_create(&desc, &platform);
    if (status != KB_OK) {
        island->load_status = (int32_t)status;
        return 0;
    }
    island->platform = platform;

    int tty_core_ready = 0;
    int tty_n_null_ready = 0;
    for (uint32_t i = 0; i < island->configured_module_count; i++) {
        const struct termd_module_config *module_cfg = &cfg->modules[i];
        if (module_cfg->image_va == 0 || module_cfg->image_size == 0 || module_cfg->name[0] == '\0') {
            island->load_status = -22;
            return 0;
        }

        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module_cfg->image_va,
            .size = (size_t)module_cfg->image_size,
            .name = module_cfg->name,
        };
        printf("[termd] linux tty module open begin name=%s size=%llu\n",
            module_cfg->name,
            (unsigned long long)module_cfg->image_size);
        status = kb_module_open_image(&image, backend, &island->modules[i]);
        if (status != KB_OK || island->modules[i] == NULL) {
            fprintf(stderr, "[termd] linux tty module phase=open failed name=%s status=%d\n",
                module_cfg->name,
                (int)status);
            island->load_status = (int32_t)status;
            return 0;
        }
        island->loaded_module_count++;
        printf("[termd] linux tty module open ready name=%s\n", module_cfg->name);

        int init_result = 0;
        printf("[termd] linux tty module init begin name=%s\n", module_cfg->name);
        status = kb_module_call_init(island->modules[i], &init_result);
        if (status == KB_ERR_NOT_FOUND) {
            printf("[termd] linux tty module init missing name=%s\n", module_cfg->name);
            continue;
        }
        if (status != KB_OK) {
            fprintf(stderr, "[termd] linux tty module phase=init_call failed name=%s status=%d result=%d\n",
                module_cfg->name,
                (int)status,
                init_result);
            island->init_status = (int32_t)status;
            return 0;
        }
        if (init_result != 0) {
            fprintf(stderr, "[termd] linux tty module phase=init_result failed name=%s result=%d\n",
                module_cfg->name,
                init_result);
            island->init_status = (int32_t)init_result;
            return 0;
        }
        printf("[termd] linux tty module init ready name=%s\n", module_cfg->name);
        if (strcmp(module_cfg->name, "linux_tty_core.ko") == 0) {
            tty_core_ready = 1;
        } else if (strcmp(module_cfg->name, "linux_tty_n_null.ko") == 0) {
            tty_n_null_ready = 1;
        }
    }

    island->compiled_source_count = island->source_count;
    island->ready = tty_core_ready &&
        tty_n_null_ready &&
        island->load_status == 0 &&
        island->init_status == 0;
    if (island->ready) {
        const int devpts_status = termd_linux_tty_island_mount_devpts(island);
        if (devpts_status != 0) {
            island->ready = 0;
        }
    }
    const int ptmx_status = termd_linux_tty_island_refresh_ptmx(island);
    if (ptmx_status == 0) {
        printf(
            "[termd] linux tty ptmx cdev ready dev=%u:%u cdev=%p fops=%p open=%p\n",
            (unsigned)TERMD_LINUX_TTYAUX_MAJOR,
            (unsigned)TERMD_LINUX_PTMX_MINOR,
            island->ptmx_cdev,
            island->ptmx_fops,
            island->ptmx_open);
    } else {
        printf(
            "[termd] linux tty ptmx cdev missing dev=%u:%u status=%d\n",
            (unsigned)TERMD_LINUX_TTYAUX_MAJOR,
            (unsigned)TERMD_LINUX_PTMX_MINOR,
            ptmx_status);
    }
    if (cfg->device_fd >= 16) {
        const int hvc_status = wait_hvc0_cdev_ready();
        if (hvc_status != 0) {
            printf("[termd] linux tty hvc0 wait failed status=%d\n", hvc_status);
        }
    }
    log_hvc0_cdev_state();
    return 0;
}
