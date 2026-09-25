/* SPDX-License-Identifier: MIT */
/* Native VM transport. Linux owns the mappings' policy and page lifetime. */
#include "vm_internal.h"
#include <errno.h>

uint64_t ph_vm_lock_space(struct ph_vm_space *space) {
    uint64_t mask;

    PH_OK(ph_notifications_save(&mask));
    ph_lock(&space->lock);
    return mask;
}

void ph_vm_unlock_space(struct ph_vm_space *space, uint64_t mask) {
    ph_unlock(&space->lock);
    PH_OK(ph_notifications_restore(mask));
}

static void notify_linux(void) {
    /* This is an event-source IRQ, not a choice of client task or guest CPU. */
    PH_OK(ph_task_ops.cpu_notify(0, KOBOX_LINUX_TASK_VM_EVENT));
}

static bool reap_if_terminal(struct ph_vm_space *space, uint64_t status[4]) {
    long result = pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT,
        space->process_fd, (uintptr_t)status);

    if (result == PACHA_SYSCALL_ERR_NOT_READY) {
        return false;
    }
    PH_OK(result);
    PH_CHECK(status[0] == 2 || status[0] == 3);
    space->reaped = true;
    space->running = false;
    return true;
}

static void queue_exit(struct ph_vm_space *space, const uint64_t status[4]) {
    /* The VM machine contract uses wait-status encoding. Native state 3 is
     * forced termination; its native code is not a POSIX signal number. */
    int32_t wait_status = status[0] == 3 ? 9 : (int32_t)((status[1] & 0xff) << 8);

    space->event = (struct kobox_linux_vm_event) {
        .kind = KOBOX_VM_EVENT_EXIT,
        .sequence = space->sequence,
        .exit_status = wait_status,
    };
    space->pending = true;
}

static void queue_client_event(struct ph_vm_space *space) {
    const struct ph_vm_client_control *control = space->control;
    uint64_t sequence = atomic_load_explicit(&control->event_sequence, memory_order_acquire);

    PH_CHECK(space->running && !space->pending);
    PH_CHECK(sequence == space->sequence + space->bootstrap_sequence);
    space->event = (struct kobox_linux_vm_event) {
        .sequence = space->sequence,
        .value = control->result,
    };
    if (control->event == PH_VM_CLIENT_DONE) {
        space->event.kind = KOBOX_VM_EVENT_STOP;
    } else if (control->event == PH_VM_CLIENT_SYSCALL_ENTRY) {
        ph_vm_capture_syscall(space);
    } else {
        PH_CHECK(control->event == PH_VM_CLIENT_FAULT && control->fault_vector == 14);
#ifdef PH_EXEC_GATE
        if (ph_exec_capture_syscall_fault(space)) {
            space->running = false;
            space->pending = true;
            return;
        }
        ph_number("exec fault address", control->fault_address);
        ph_number("exec fault IP", control->fault_ip);
        ph_number("exec fault flags", control->fault_flags);
        ph_number("exec fault error", control->fault_error);
#endif
        space->event.kind = KOBOX_VM_EVENT_FAULT;
        space->event.fault = (struct kobox_linux_vm_fault) {
            .address = control->fault_address,
            .ip = control->fault_ip,
            .sp = control->fault_sp,
            .flags = control->fault_flags,
            .error = ph_vm_guest_fault_error(space),
        };
        ph_vm_capture_fault(space);
    }
    space->running = false;
    space->pending = true;
}

static void *monitor_client(void *opaque) {
    struct ph_vm_space *space = opaque;
    struct pacha_pollfd descriptors[2] = {
        {.fd = space->event_fd, .events = PACHA_FD_EVENT_READABLE},
        {.fd = space->process_fd, .events = PACHA_FD_EVENT_READABLE},
    };

    for (;;) {
        long ready = ph_wait_readable(descriptors, 2);

        PH_CHECK(ready >= 1 && ready <= 2);
        uint64_t memory_mask = ph_vm_lock_memory(space);
        uint64_t mask = ph_vm_lock_space(space);
        uint64_t status[4];
        bool terminal = reap_if_terminal(space, status);

        if (terminal) {
            queue_exit(space, status);
        } else {
            PH_CHECK(descriptors[0].revents == PACHA_FD_EVENT_READABLE);
            ph_vm_event_consume(space->event_fd);
            queue_client_event(space);
        }
        ph_vm_unlock_space(space, mask);
        ph_vm_unlock_memory(space, memory_mask);
        notify_linux();
        if (terminal) {
            return NULL;
        }
    }
}

static int close_space(void *opaque) {
    struct ph_vm_space *space = opaque;
    uint64_t mask = ph_vm_lock_space(space);

    if (!space->reaped) {
        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, space->process_fd, 0));
        uint64_t status[4];

        while (!reap_if_terminal(space, status)) {
            struct pacha_pollfd completion = {
                .fd = space->process_fd,
                .events = PACHA_FD_EVENT_READABLE,
            };

            PH_CHECK(ph_wait_readable(&completion, 1) == 1);
        }
        /* The native producer queues the single EXIT and wakes Linux after
         * this lock is released. close itself only guarantees actual reaping. */
    }
    bool drain = !atomic_load_explicit(&space->cleanup_state, memory_order_relaxed);

    if (drain) {
        atomic_store_explicit(&space->cleanup_state, 1, memory_order_relaxed);
    }
    ph_vm_unlock_space(space, mask);
    if (drain) {
        /* Reaping is not enough: stop the native producer before closing
         * descriptors or its control alias. Keep only the Linux-facing
         * tombstone/MM membership until all core bindings are gone. */
        PH_OK(ph_thread_join(space->monitor));
        space->monitor = NULL;
        int *descriptors[] = {&space->thread_fd, &space->process_fd,
            &space->kick_fd, &space->event_fd, &space->control_fd};

        for (unsigned index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]); ++index) {
            PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, *descriptors[index]));
            *descriptors[index] = -1;
        }
        for (unsigned protection = 0; protection < 8; ++protection) {
            PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, space->ram_grant_fds[protection]));
            space->ram_grant_fds[protection] = -1;
        }
        ph_free(space->control, PH_PAGE_SIZE);
        space->control = NULL;
        atomic_store_explicit(&space->cleanup_state, 2, memory_order_release);
        ph_wake(&space->cleanup_state);
    } else {
        while (atomic_load_explicit(&space->cleanup_state, memory_order_acquire) != 2) {
            ph_wait(&space->cleanup_state, 1);
        }
    }
    return 0;
}

void ph_vm_publish_command(struct ph_vm_space *space, enum ph_vm_client_command command) {
    struct ph_vm_client_control *control = space->control;

    control->command = command;
    space->running = true;
    ++space->sequence;
    atomic_store_explicit(&control->command_sequence,
        space->sequence + space->bootstrap_sequence, memory_order_release);
    if (!(space->native_stopped && space->user_started)) {
        ph_vm_event_signal(space->kick_fd);
    }
    if (space->native_stopped) {
        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, space->process_fd, 0));
        space->native_stopped = false;
    }
}

static int submit(struct ph_vm_space *space, uint64_t sequence, bool resume,
    uint64_t address, unsigned write, uint64_t value) {
    uint64_t mask = ph_vm_lock_space(space);
    int result = ph_vm_idle_status(space, sequence);

    if (!result && space->syscall_stopped && (!resume || !space->syscall_returned)) {
        result = -EBUSY;
    }
    if (!result && space->fault_stopped && !resume) {
        result = -EBUSY;
    }
    if (!result) {
        struct ph_vm_client_control *control = space->control;

        control->address = address;
        control->write = write;
        control->value = value;
        space->syscall_stopped = false;
        space->fault_stopped = false;
        space->syscall_returned = false;
        space->syscall_in_fault = false;
        ph_vm_publish_command(space, resume ? PH_VM_CLIENT_RESUME : PH_VM_CLIENT_ACCESS);
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

int ph_vm_probe(void *opaque, uint64_t address, unsigned write, uint64_t value, uint64_t sequence) {
    struct ph_vm_space *space = opaque;

    /* Segment probes execute real native FS/GS accesses after Linux arch_prctl. */
    if (write > 1 && (write < 5 || write > 11)) {
        return -EOPNOTSUPP;
    }
    if (address < space->start || address - space->start >= space->length ||
        sizeof(uint64_t) > space->length - (address - space->start)) {
        return -EINVAL;
    }
    if ((write == 7 || write == 9 || write == 10) &&
        4 * PH_PAGE_SIZE > space->length - (address - space->start)) {
        return -EINVAL;
    }
    if (write == 11 && (value >= PH_CPU_COUNT ||
        32 * PH_PAGE_SIZE > space->length - (address - space->start))) {
        return -EINVAL;
    }
    return submit(space, sequence, false, address, write, value);
}

static int resume_space(void *opaque, uint64_t sequence) {
    return submit(opaque, sequence, true, 0, 0, 0);
}

static int next_event(void *opaque, struct kobox_linux_vm_event *event) {
    struct ph_vm_space *space = opaque;
    uint64_t mask = ph_vm_lock_space(space);
    int result = space->exit_delivered ? -ESRCH : -EAGAIN;

    if (space->pending) {
        *event = space->event;
        space->pending = false;
        if (event->kind == KOBOX_VM_EVENT_EXIT) {
            space->exit_delivered = true;
        }
        result = 0;
    }
    ph_vm_unlock_space(space, mask);
    return result;
}

const struct kobox_linux_vm_host_operations ph_vm_ops = {
    .size = sizeof(ph_vm_ops),
    .map = ph_vm_map_pages,
    .reset = ph_vm_reset_pages,
    .close = close_space,
    .clone = ph_vm_clone,
    .enable_syscalls = ph_vm_enable_syscalls,
    .start = ph_vm_start,
    .snapshot = ph_vm_snapshot,
    .restore = ph_vm_restore,
    .write_fpregs = ph_vm_write_fpregs,
    .syscall_return = ph_vm_syscall_return,
    .resume = resume_space,
    .event = next_event,
};

static int create_event_counter(void) {
    uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE, 0, rights, 0);

    PH_CHECK(fd >= 16);
    return (int)fd;
}

static void create_ram_grants(struct ph_vm_space *space) {
    for (unsigned protection = 0; protection < 8; ++protection) {
        uint64_t rights = PACHA_FD_RIGHT_CLOSE;

        if (protection & KOBOX_VM_READ) {
            rights |= PACHA_FD_RIGHT_MAP_READ;
        }
        if (protection & KOBOX_VM_WRITE) {
            rights |= PACHA_FD_RIGHT_MAP_WRITE;
        }
        if (protection & KOBOX_VM_EXECUTE) {
            rights |= PACHA_FD_RIGHT_MAP_EXEC;
        }
        long fd = pacha_syscall4(PACHA_FD_SYSCALL_DUP, ph_core.ram_fd, 16,
            rights, PACHA_FD_FLAG_PRIVATE);

        PH_CHECK(fd >= 16);
        space->ram_grant_fds[protection] = (int)fd;
    }
}

static void verify_native_protection(struct ph_vm_space *space) {
    struct pacha_pollfd completion = {
        .fd = space->event_fd,
        .events = PACHA_FD_EVENT_READABLE,
    };

    /* Before exposing the handle to Linux, try all native permission changes
     * on a reserved RAM page. No instruction accesses or modifies that page.
     * The child has no RAM FD and must not raise a mapping's granted ceiling. */
    for (unsigned granted = 0; granted < 8; ++granted) {
        PH_OK(ph_vm_map_pages(space, space->start, 0, PH_PAGE_SIZE, granted));
        for (unsigned requested = 0; requested < 8; ++requested) {
            struct ph_vm_client_control *control = space->control;

            control->command = PH_VM_CLIENT_PROTECT;
            control->address = space->start;
            control->value = requested;
            atomic_store_explicit(&control->command_sequence, ++space->bootstrap_sequence,
                memory_order_release);
            ph_vm_event_signal(space->kick_fd);
            PH_CHECK(ph_wait_readable(&completion, 1) == 1);
            ph_vm_event_consume(space->event_fd);
            PH_CHECK(atomic_load_explicit(&control->event_sequence, memory_order_acquire) ==
                space->bootstrap_sequence);
            PH_CHECK(control->event == PH_VM_CLIENT_DONE);
            uint64_t expected = (requested & ~granted) ? PACHA_SYSCALL_ERR_INVALID : 0;

            if (control->result != expected) {
                ph_number("VM granted protection", granted);
                ph_number("VM requested protection", requested);
                ph_fail(__FILE__, __LINE__, control->result);
            }
        }
    }
    PH_OK(ph_vm_reset_pages(space, space->start, PH_PAGE_SIZE));
    ph_log("PACHA_KOBOX_VM_NATIVE_PROTECTION=PASS\n");
}

struct ph_vm_space *ph_vm_create(const void *file, size_t file_size,
    uint64_t start, uint64_t length) {
    PH_CHECK(start >= KOBOX_X86_USER_START && start < KOBOX_X86_USER_END && length);
    PH_CHECK(!((start | length) & (PH_PAGE_SIZE - 1)));
    PH_CHECK(length <= KOBOX_X86_USER_END - start);
    struct ph_vm_space *space = ph_alloc(sizeof(*space));

    space->start = start;
    space->length = length;
    space->bootstrap_file = file;
    space->bootstrap_size = file_size;
    space->owner = space;
    ph_vm_memory_create(space);
    create_ram_grants(space);
    space->kick_fd = create_event_counter();
    space->event_fd = create_event_counter();
    struct pacha_process_fd_grant grants[] = {
        {.source_fd = space->kick_fd, .target_fd = PH_VM_CLIENT_KICK_FD,
            .rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_POLL},
        {.source_fd = space->event_fd, .target_fd = PH_VM_CLIENT_EVENT_FD,
            .rights = PACHA_FD_RIGHT_WRITE},
    };
    uint64_t process_rights = PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    long process = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, process_rights, 0,
        (uintptr_t)grants, sizeof(grants) / sizeof(grants[0]));

    PH_CHECK(process >= 16);
    space->process_fd = (int)process;
    uint64_t entry = ph_vm_load_bootstrap(space->process_fd, file, file_size);
    uint64_t control_rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_CLOSE;
    long backing = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PH_PAGE_SIZE, control_rights, 0);

    PH_CHECK(backing >= 16);
    space->control_fd = (int)backing;
    long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, backing, 0, PH_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);

    PH_CHECK(alias >= (long)PH_PAGE_SIZE);
    space->control = (void *)(uintptr_t)alias;
    PH_CHECK((uint64_t)pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, backing,
        PH_VM_CLIENT_CONTROL, PH_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_PROCESS_MAP_SHARED) == PH_VM_CLIENT_CONTROL);
    ph_vm_map_anonymous(process, PH_VM_CLIENT_STACK, PH_VM_CLIENT_STACK_SIZE);

    long thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, process, entry,
        PH_VM_CLIENT_STACK + PH_VM_CLIENT_STACK_SIZE, 0, 0,
        PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SET_CONTEXT);

    if (thread < 16) {
        ph_fail(__FILE__, __LINE__, thread);
    }
    space->thread_fd = (int)thread;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_START, thread));
    struct pacha_pollfd ready[2] = {
        {.fd = space->event_fd, .events = PACHA_FD_EVENT_READABLE},
        {.fd = space->process_fd, .events = PACHA_FD_EVENT_READABLE},
    };
    PH_CHECK(ph_wait_readable(ready, 2) == 1);
    if (ready[1].revents) {
        uint64_t status[4];

        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, process, (uintptr_t)status));
        ph_number("native VM bootstrap exit", status[1]);
    }
    PH_CHECK(ready[0].revents == PACHA_FD_EVENT_READABLE && !ready[1].revents);
    ph_vm_event_consume(space->event_fd);
    PH_CHECK(atomic_load_explicit(&space->control->event_sequence, memory_order_acquire) == 0);
    PH_CHECK(space->control->event == PH_VM_CLIENT_READY && space->control->pid != 0);
    space->pid = space->control->pid;
    ph_vm_install_lpr_entry(space);
    verify_native_protection(space);
    ph_vm_park_bootstrap(space);
    space->monitor = ph_thread_create(monitor_client, space);
    return space;
}

uint64_t ph_vm_pid(const struct ph_vm_space *space) {
    return space->pid;
}

int ph_vm_terminate(struct ph_vm_space *space) {
    uint64_t mask = ph_vm_lock_space(space);
    int result = space->reaped ? -ESRCH :
        ph_vm_native_error(pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, space->process_fd, 0));

    ph_vm_unlock_space(space, mask);
    return result;
}

void ph_vm_destroy(struct ph_vm_space *space) {
    while (space->owned_children) {
        struct ph_vm_space *child = space->owned_children;

        space->owned_children = child->owned_next;
        ph_vm_destroy(child);
    }
    PH_OK(close_space(space));
    ph_vm_memory_destroy(space);
    ph_free(space, sizeof(*space));
}
