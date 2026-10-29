/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "bootstrap.h"
#include "catalog.h"
#include "disk_port.h"
#include "execute.h"
#include "menu.h"
#include "tty.h"

#include <pacha/bootstrap.h>
#include <pacha/ipc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct installer_runtime {
    const struct pacha_installer_bootstrap *boot;
    struct installer_block_source sources[PACHA_INSTALLER_MAX_SOURCES];
    struct installer_catalog_entry entries[32];
    struct installer_disk displays[32];
    size_t count;
    size_t media_index;
    uint64_t request_id;
    const char *diagnostic_phase;
};

struct runner_progress {
    installer_progress report;
    void *context;
    uint32_t flags;
    unsigned percent;
    uint32_t phase;
    uint32_t operation;
    unsigned port_kind;
    unsigned base;
    unsigned span;
    uint64_t completed_bytes;
    uint64_t total_bytes;
    uint64_t last_report_bytes;
    const char *label;
};

static void runner_progress_emit(struct runner_progress *state,
    unsigned percent, const char *phase) {
    if (!state || !state->report) return;
    if (percent < state->percent) percent = state->percent;
    if (percent > 100) percent = 100;
    state->percent = percent;
    state->report(state->context, percent, phase);
}

static int runner_progress_phase(struct runner_progress *state,
    const struct installer_progress_wire *wire) {
    if (!state || !wire || wire->phase <= state->phase) return 0;
    if ((state->flags & PACHA_INSTALLER_RUNNER_PREFLIGHT) != 0) {
        if (wire->phase > INSTALLER_PROGRESS_PREFLIGHT_ROOTFS) return 0;
    } else if (wire->phase < INSTALLER_PROGRESS_RECHECK_FILES) return 0;
    state->phase = wire->phase;
    state->completed_bytes = wire->completed_bytes;
    state->total_bytes = wire->total_bytes;
    state->last_report_bytes = 0;
    state->port_kind = 1;
    state->operation = INSTALLER_DISK_PORT_READ;
    switch (wire->phase) {
    case INSTALLER_PROGRESS_PREFLIGHT_FILES:
        state->base = 1; state->span = 5;
        state->label = "Preparing boot payload files"; break;
    case INSTALLER_PROGRESS_PREFLIGHT_ROOTFS:
        state->base = 6; state->span = 12;
        state->label = "Hashing the root filesystem archive"; break;
    case INSTALLER_PROGRESS_RECHECK_FILES:
        state->base = 30; state->span = 4;
        state->label = "Rechecking boot payload files"; break;
    case INSTALLER_PROGRESS_RECHECK_ROOTFS:
        state->base = 34; state->span = 8;
        state->label = "Rechecking the root filesystem archive"; break;
    case INSTALLER_PROGRESS_EXTRACT_ROOTFS:
        state->base = 42; state->span = 12;
        state->label = "Extracting the root filesystem"; break;
    case INSTALLER_PROGRESS_FORMAT_EXT4:
        state->base = 54; state->span = 19; state->port_kind = 2;
        state->operation = INSTALLER_DISK_PORT_WRITE;
        state->label = "Creating and populating ext4"; break;
    case INSTALLER_PROGRESS_VERIFY_EXT4:
        state->base = 73; state->span = 9; state->port_kind = 2;
        state->label = "Checking the ext4 filesystem"; break;
    case INSTALLER_PROGRESS_FORMAT_ESP:
        state->base = 82; state->span = 4; state->port_kind = 0;
        state->operation = INSTALLER_DISK_PORT_WRITE;
        state->label = "Creating the FAT32 EFI partition"; break;
    case INSTALLER_PROGRESS_DEPLOY_UEFI:
        state->base = 86; state->span = 7; state->port_kind = 0;
        state->operation = INSTALLER_DISK_PORT_WRITE;
        state->label = "Installing UEFI boot files"; break;
    case INSTALLER_PROGRESS_VERIFY_UEFI:
        state->base = 93; state->span = 4; state->port_kind = 0;
        state->label = "Reading back UEFI boot files"; break;
    case INSTALLER_PROGRESS_COMPLETE:
        state->base = 97; state->span = 1; state->port_kind = 3;
        state->operation = 0;
        state->label = "Filesystem installation verified"; break;
    default: return 0;
    }
    fprintf(stderr,
        "[installer] progress phase=%u base=%u span=%u total=%llu label=%s\n",
        wire->phase, state->base, state->span,
        (unsigned long long)state->total_bytes, state->label);
    fflush(stderr);
    runner_progress_emit(state, state->base, state->label);
    return 1;
}

static void runner_progress_bytes(struct runner_progress *state,
    uint32_t transferred) {
    if (!state || !transferred || !state->label) return;
    state->completed_bytes += transferred;
    unsigned percent = state->base;
    if (state->total_bytes && state->span) {
        uint64_t completed = state->completed_bytes;
        if (completed > state->total_bytes) completed = state->total_bytes;
        unsigned step = (unsigned)((completed * state->span) /
            state->total_bytes);
        if (step >= state->span) step = state->span - 1;
        percent += step;
    }
    if (percent == state->percent &&
        state->completed_bytes - state->last_report_bytes <
            (UINT64_C(1) << 20)) return;
    state->last_report_bytes = state->completed_bytes;
    char phase[128];
    if (state->phase == INSTALLER_PROGRESS_FORMAT_EXT4) {
        /* The archive size excludes filesystem metadata and journal writes;
         * presenting it as the total would falsely show completed work. */
        snprintf(phase, sizeof(phase), "ext4: %llu.%01llu MiB written (data + metadata)",
            (unsigned long long)(state->completed_bytes >> 20),
            (unsigned long long)((state->completed_bytes & 0xfffff) * 10 >> 20));
    } else if (state->total_bytes) {
        uint64_t shown = state->completed_bytes;
        if (shown > state->total_bytes) shown = state->total_bytes;
        snprintf(phase, sizeof(phase), "%s: %llu.%01llu / %llu.%01llu MiB",
            state->label,
            (unsigned long long)(shown >> 20),
            (unsigned long long)((shown & 0xfffff) * 10 >> 20),
            (unsigned long long)(state->total_bytes >> 20),
            (unsigned long long)((state->total_bytes & 0xfffff) * 10 >> 20));
    } else {
        snprintf(phase, sizeof(phase), "%s: %llu.%01llu MiB processed",
            state->label,
            (unsigned long long)(state->completed_bytes >> 20),
            (unsigned long long)((state->completed_bytes & 0xfffff) * 10 >> 20));
    }
    runner_progress_emit(state, percent, phase);
}

static void runner_progress_note(struct runner_progress *state,
    unsigned port_kind, const struct installer_disk_port *port) {
    if (!state || !port || port->completed_status ||
        port->completed_operation == INSTALLER_DISK_PORT_ATTACH) return;
    const uint32_t operation = port->completed_operation;
    const uint32_t bytes = port->completed_transferred;
    if (operation == INSTALLER_DISK_PORT_PROGRESS && port_kind == 1) {
        (void)runner_progress_phase(state, &port->completed_progress);
        return;
    }
    if (port_kind == state->port_kind && operation == state->operation)
        runner_progress_bytes(state, bytes);
}

static int catalog_has_install_target(const struct installer_runtime *runtime) {
    if (!runtime || runtime->media_index >= runtime->count) return 0;
    for (size_t i = 0; i < runtime->count; ++i) {
        const kb2_block_disk_t *disk = &runtime->entries[i].target.disk;
        if (i != runtime->media_index && !disk->read_only && disk->diskseq &&
            disk->bytes && disk->logical_block_size) return 1;
    }
    return 0;
}

static int wait_for_catalog(struct installer_runtime *runtime,
    uint32_t source_count) {
    if (!runtime || !source_count) return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    const pacha_fd_t timer = pacha_timerfd_create(UINT64_C(100000000),
        UINT64_C(100000000), rights, 0);
    if (timer < 16) return timer < 0 ? timer : -EIO;
    int status = -ETIMEDOUT;
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        runtime->count = 0;
        runtime->media_index = 0;
        status = installer_catalog_scan(runtime->sources, source_count,
            runtime->entries,
            sizeof(runtime->entries) / sizeof(runtime->entries[0]),
            &runtime->count, &runtime->media_index);
        if (!status && catalog_has_install_target(runtime)) break;
        /* USB mass storage and NVMe namespaces appear after the controller's
         * service-ready event.  Retry only an incomplete initial catalog or
         * a generation rollover; structural/media errors still fail closed. */
        if (!status || status == -ENODEV || status == -ESTALE) {
            status = -ETIMEDOUT;
        } else {
            break;
        }
        struct pacha_pollfd tick = {.fd = timer,
            .events = PACHA_FD_EVENT_READABLE};
        uint64_t expirations = 0;
        if (pacha_fd_wait_many(&tick, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
            pacha_fd_read(timer, &expirations, sizeof(expirations)) !=
                (long)sizeof(expirations)) {
            status = -EIO;
            break;
        }
    }
    (void)pacha_fd_close(timer);
    return status;
}

static int checked_extent(const struct installer_extent extent,
    uint32_t block, uint64_t disk_bytes, uint64_t *offset, uint64_t *bytes) {
    if (!block || extent.first_lba > extent.last_lba ||
        extent.first_lba > UINT64_MAX / block ||
        extent.last_lba == UINT64_MAX ||
        extent.last_lba + 1 > UINT64_MAX / block) return -EOVERFLOW;
    *offset = extent.first_lba * block;
    *bytes = (extent.last_lba - extent.first_lba + 1) * (uint64_t)block;
    return *offset <= disk_bytes && *bytes <= disk_bytes - *offset ? 0 : -ERANGE;
}

static int media_layout(struct installer_runtime *runtime,
    struct pacha_installer_runner_plan *request) {
    const struct installer_catalog_entry *media =
        &runtime->entries[runtime->media_index];
    const struct installer_gpt *gpt = &media->gpt;
    const uint32_t block = media->target.disk.logical_block_size;
    size_t esp_count = 0, data_count = 0;
    for (size_t i = 0; i < gpt->partition_count; ++i) {
        const struct installer_partition *p = &gpt->partitions[i];
        if (installer_guid_equal(p->type_guid, installer_esp_type_guid)) {
            ++esp_count;
            request->media_esp_offset = p->first_lba * (uint64_t)block;
        }
        if (!strcmp(p->name, "pacha-install-data")) {
            ++data_count;
            request->media_data_offset = p->first_lba * (uint64_t)block;
        }
    }
    request->media_bytes = media->target.disk.bytes;
    return esp_count == 1 && data_count == 1 &&
        request->media_esp_offset < request->media_bytes &&
        request->media_data_offset < request->media_bytes ? 0 : -EINVAL;
}

static int make_port_pair(struct pacha_ipc_channel_pair *pair) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    return pacha_ipc_channel_create(pair, rights, 0);
}

static int wait_runner_done(struct installer_runtime *runtime, uint64_t id) {
    struct pacha_ipc_msg reply = {0};
    const int status = pacha_ipc_recv_wait(
        (pacha_fd_t)runtime->boot->control_fd, &reply, PACHA_FD_WAIT_FOREVER);
    if (status) return status;
    if (reply.fd_count || reply.word0 != PACHA_INSTALLER_MSG_RUNNER_DONE ||
        reply.word1 != id) return -EPROTO;
    return (int)(int64_t)reply.word2;
}

static int run_linux_tools(struct installer_runtime *runtime,
    struct installer_block_target *target, const struct installer_plan *plan,
    uint32_t flags, installer_progress progress, void *progress_context) {
    struct pacha_installer_runner_plan wire = {
        .magic = PACHA_INSTALLER_MSG_PLAN,
        .size = sizeof(struct pacha_installer_runner_plan),
        .flags = flags,
    };
    runtime->diagnostic_phase = "media-layout";
    int status = media_layout(runtime, &wire);
    if (status) return status;
    if (!pacha_installer_runner_flags_valid(flags)) return -EINVAL;
    if (!pacha_installer_runner_read_only(flags)) {
        if (!target || !plan) return -EINVAL;
        wire.target_bytes = target->disk.bytes;
        status = checked_extent(plan->esp, plan->logical_block,
            wire.target_bytes, &wire.esp_offset, &wire.esp_bytes);
        if (!status) status = checked_extent(plan->root,
            plan->logical_block, wire.target_bytes,
            &wire.root_offset, &wire.root_bytes);
        if (status) return status;
    }

    struct pacha_ipc_channel_pair target_pair = {.a = -1, .b = -1};
    struct pacha_ipc_channel_pair media_pair = {.a = -1, .b = -1};
    struct pacha_ipc_channel_pair root_pair = {.a = -1, .b = -1};
    runtime->diagnostic_phase = "target-port";
    status = make_port_pair(&target_pair);
    if (status) goto out;
    runtime->diagnostic_phase = "media-port";
    status = make_port_pair(&media_pair);
    if (status) goto out;
    runtime->diagnostic_phase = "root-port";
    status = make_port_pair(&root_pair);
    if (status) goto out;
    struct installer_block_target media_target =
        runtime->entries[runtime->media_index].target;
    struct installer_disk_port target_port = {
        /* Formatters must size their allocation units from the partition,
         * not from the parent SSD. This also confines ESP writes. */
        .target = target, .visible_first = wire.esp_offset,
        .visible_end = wire.esp_offset + wire.esp_bytes,
        .writable_first = 0, .writable_end = wire.esp_bytes,
        .fd = target_pair.a, .page_fd = -1};
    struct installer_disk_port media_port = {
        .target = &media_target, .writable_first = 0, .writable_end = 0,
        .allow_progress = 1,
        .fd = media_pair.a, .page_fd = -1};
    struct installer_disk_port root_port = {
        .target = target, .visible_first = wire.root_offset,
        .visible_end = wire.root_offset + wire.root_bytes,
        .writable_first = 0, .writable_end = wire.root_bytes,
        .fd = root_pair.a, .page_fd = -1};
    target_pair.a = media_pair.a = root_pair.a = -1;

    const uint64_t vmo_rights = PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    runtime->diagnostic_phase = "plan-vmo";
    pacha_fd_t plan_fd = pacha_vmo_create(4096, vmo_rights, 0);
    if (plan_fd < 16) { status = plan_fd < 0 ? plan_fd : -EIO; goto ports; }
    runtime->diagnostic_phase = "plan-map";
    void *mapped = pacha_mmap(plan_fd, 4096,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!mapped) { status = -ENOMEM; goto plan; }
    memset(mapped, 0, 4096);
    memcpy(mapped, &wire, sizeof(wire));
    (void)pacha_munmap(mapped, 4096);

    struct pacha_ipc_fd fds[4] = {
        {.fd = (uint64_t)plan_fd, .rights = vmo_rights},
        {.fd = (uint64_t)target_pair.b,
         .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL},
        {.fd = (uint64_t)media_pair.b,
         .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL},
        {.fd = (uint64_t)root_pair.b,
         .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL},
    };
    const uint64_t id = ++runtime->request_id;
    const struct pacha_ipc_msg message = {
        .word0 = PACHA_INSTALLER_MSG_PLAN, .word1 = id,
        .fds = fds, .fd_count = 4};
    runtime->diagnostic_phase = "runner-launch";
    status = pacha_ipc_send((pacha_fd_t)runtime->boot->control_fd, &message);
    (void)pacha_fd_close(target_pair.b); target_pair.b = -1;
    (void)pacha_fd_close(media_pair.b); media_pair.b = -1;
    (void)pacha_fd_close(root_pair.b); root_pair.b = -1;
    if (status) goto plan;

    runtime->diagnostic_phase = flags & PACHA_INSTALLER_RUNNER_PREFLIGHT ?
        "payload-preflight" : "filesystem-tools";
    struct runner_progress tracker = {
        .report = progress, .context = progress_context, .flags = flags,
        .percent = flags & PACHA_INSTALLER_RUNNER_PREFLIGHT ? 1 : 30,
    };
    fprintf(stderr,
        "[installer] filesystem runner progress attached flags=0x%x initial=%u\n",
        flags, tracker.percent);
    fflush(stderr);
    runner_progress_emit(&tracker, tracker.percent,
        flags & PACHA_INSTALLER_RUNNER_PREFLIGHT ?
            "Verifying installation media" :
            "Reading and verifying installation payload");
    for (;;) {
        struct pacha_pollfd events[4] = {
            {.fd = target_port.fd,
             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = media_port.fd,
             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = (pacha_fd_t)runtime->boot->control_fd,
             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = root_port.fd,
             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        };
        long ready = pacha_fd_wait_many(events, 4, PACHA_FD_WAIT_FOREVER);
        if (ready <= 0) { status = -EIO; break; }
        if (events[2].revents) {
            status = wait_runner_done(runtime, id);
            break;
        }
        if (events[0].revents) {
            if (events[0].revents & PACHA_FD_EVENT_HANGUP) {
                status = wait_runner_done(runtime, id); break;
            }
            status = installer_disk_port_pump(&target_port);
            if (!status) runner_progress_note(&tracker, 0, &target_port);
            if (status) break;
        }
        if (events[1].revents) {
            if (events[1].revents & PACHA_FD_EVENT_HANGUP) {
                status = wait_runner_done(runtime, id); break;
            }
            status = installer_disk_port_pump(&media_port);
            if (!status) runner_progress_note(&tracker, 1, &media_port);
            if (status) break;
        }
        if (events[3].revents) {
            if (events[3].revents & PACHA_FD_EVENT_HANGUP) {
                status = wait_runner_done(runtime, id); break;
            }
            status = installer_disk_port_pump(&root_port);
            if (!status) runner_progress_note(&tracker, 2, &root_port);
            if (status) break;
        }
    }
plan:
    if (plan_fd >= 16) (void)pacha_fd_close(plan_fd);
ports:
    installer_disk_port_close(&target_port);
    installer_disk_port_close(&media_port);
    installer_disk_port_close(&root_port);
out:
    if (target_pair.a >= 16) (void)pacha_fd_close(target_pair.a);
    if (target_pair.b >= 16) (void)pacha_fd_close(target_pair.b);
    if (media_pair.a >= 16) (void)pacha_fd_close(media_pair.a);
    if (media_pair.b >= 16) (void)pacha_fd_close(media_pair.b);
    if (root_pair.a >= 16) (void)pacha_fd_close(root_pair.a);
    if (root_pair.b >= 16) (void)pacha_fd_close(root_pair.b);
    return status;
}

static int tool_preflight(void *context, installer_progress progress,
    void *progress_context) {
    return run_linux_tools(context, NULL, NULL,
        PACHA_INSTALLER_RUNNER_PREFLIGHT, progress, progress_context);
}

static int tool_run_plan(void *context, struct installer_block_target *target,
    const struct installer_plan *plan, installer_progress progress,
    void *progress_context) {
    uint32_t flags = (plan->format_esp ?
        PACHA_INSTALLER_RUNNER_FORMAT_ESP : 0) |
        (plan->format_root ? PACHA_INSTALLER_RUNNER_FORMAT_ROOT : 0);
    return run_linux_tools(context, target, plan, flags, progress,
        progress_context);
}

static void send_complete(const struct pacha_installer_bootstrap *boot,
    uint64_t result, int status) {
    const struct pacha_ipc_msg message = {
        .word0 = PACHA_INSTALLER_MSG_COMPLETE, .word1 = result,
        .word2 = (uint64_t)(int64_t)status};
    (void)pacha_ipc_send((pacha_fd_t)boot->ready_fd, &message);
}

int main(int argc, char **argv) {
    (void)argc;
    const pacha_fd_t bootstrap_fd = pacha_bootstrap_fd_from_argv(argv);
    struct pacha_installer_bootstrap boot = {0};
    if (bootstrap_fd < 16 ||
        pacha_fd_read(bootstrap_fd, &boot, sizeof(boot)) != (long)sizeof(boot) ||
        boot.magic != PACHA_INSTALLER_BOOT_MAGIC ||
        boot.size != sizeof(boot) ||
        !boot.source_count || boot.source_count > PACHA_INSTALLER_MAX_SOURCES ||
        boot.termd_fd < 16 || boot.ready_fd < 16 || boot.control_fd < 16)
        return 1;

    /* A catalog contains full GPT snapshots for every candidate and is far
     * larger than the deliberately small native-process stack.  It also
     * lives for the whole installer session, so keep it in process heap
     * storage instead of making main's stack frame hardware-dependent. */
    struct installer_runtime *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return 1;
    runtime->boot = &boot;
    int status = 0;
    for (uint32_t i = 0; i < boot.source_count; ++i) {
        status = installer_block_source_open(&runtime->sources[i],
            (pacha_fd_t)boot.sources[i].fd, boot.sources[i].resource_id);
        if (status) break;
    }
    if (!status) status = wait_for_catalog(runtime, boot.source_count);

    for (size_t i = 0; !status && i < runtime->count; ++i)
        runtime->displays[i] = runtime->entries[i].display;

    struct installer_tty tty = {.endpoint_fd = -1, .page_fd = -1,
        .notify_fd = -1};
    FILE *output = NULL;
    unsigned tty_rows = 24, tty_columns = 80;
    if (!status) status = installer_tty_open(&tty, (pacha_fd_t)boot.termd_fd,
        boot.pts_index, &output);
    if (!status && installer_tty_get_size(&tty, &tty_rows, &tty_columns)) {
        tty_rows = 24;
        tty_columns = 80;
    }
    if (!status) status = installer_tty_enter_ui(&tty);
    if (!status) {
        struct installer_execution execution = {
            .entries = runtime->entries, .count = runtime->count,
            .media_index = runtime->media_index,
            .tools = {.preflight = tool_preflight,
                .run_plan = tool_run_plan, .context = runtime},
        };
        status = installer_ui_run(installer_tty_read_key, &tty, output,
            runtime->displays, runtime->count,
            runtime->media_index, tty_rows, tty_columns,
            installer_execute_plan, &execution);
        const int restore_status = installer_tty_leave_ui(&tty);
        if (!status && restore_status) status = restore_status;
        if (status == INSTALLER_UI_SHELL) {
            installer_ui_leave_for_shell(output);
            /* The media port is read-only and no SSD is armed. The same
             * runner/broker remains responsible for launching Linux tools;
             * seed0root does not learn about desktop packages or their files. */
            int import_status = run_linux_tools(runtime, NULL, NULL,
                PACHA_INSTALLER_RUNNER_IMPORT_LIVE, NULL, NULL);
            if (import_status)
                fprintf(output, "Live package preparation failed (%d); recovery shell remains available. See /run/pacha/live-desktop.log\n",
                    import_status);
            send_complete(&boot, PACHA_INSTALLER_RESULT_SHELL, 0);
            status = 0;
        } else if (status == INSTALLER_UI_DONE) {
            installer_ui_message(output, tty_rows, tty_columns,
                "Installation Complete",
                "Reboot once with this USB inserted to register NVMe boot.",
                0);
            send_complete(&boot, PACHA_INSTALLER_RESULT_INSTALLED, 0);
            status = 0;
        } else {
            char message[128];
            snprintf(message, sizeof(message),
                "Stopped safely: error %d at %s; see /run/pacha/installer.log",
                status, runtime->diagnostic_phase ?
                    runtime->diagnostic_phase : "startup");
            installer_ui_message(output, tty_rows, tty_columns,
                "Installation Failed", message, 1);
            send_complete(&boot, 0, status);
        }
    } else send_complete(&boot, 0, status);
    if (output) fclose(output);
    installer_tty_close(&tty);
    for (uint32_t i = 0; i < boot.source_count; ++i)
        installer_block_source_close(&runtime->sources[i]);
    free(runtime);
    return status ? 1 : 0;
}
