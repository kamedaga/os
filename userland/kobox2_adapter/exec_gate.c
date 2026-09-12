/* SPDX-License-Identifier: MIT */
/* The existing Linux exec verifier owns task creation and ELF execution. */
#include "host.h"
#include "bootfs.h"
#include "boot/exec_gate.h"
#include "vm_internal.h"
#include "exec_image.h"

static void detach_image(struct ph_vm_space *space) {
    PH_CHECK(space->reaped && atomic_load_explicit(&space->cleanup_state, memory_order_acquire) == 2);
    space->exec_image = NULL;
    for (struct ph_vm_space *child = space->owned_children; child; child = child->owned_next) {
        detach_image(child);
    }
}

void ph_verify_exec_gate(const struct kobox_linux_vm_test *vm) {
    size_t length;
    const void *file = ph_bootfs_file("/srv/kobox2/client.elf", &length);
    struct ph_exec_image image = ph_exec_prepare_image(file, length);
    struct kobox_exec_test test = {
        .size = sizeof(test), .vm = vm, .image = image.bytes, .length = length,
    };
    struct kobox_exec_report report = {.size = sizeof(report)};
    int (*verify)(const struct kobox_exec_test *, struct kobox_exec_report *);
    void *address = ph_image_lookup(&ph_core, "kobox_linux_exec_verify");

    PH_CHECK(address != NULL);
    memcpy(&verify, &address, sizeof(verify));
    for (unsigned index = 0; index < 2; ++index) {
        struct ph_vm_space *space = vm->spaces[index];

        PH_CHECK(!space->user_started && space->native_stopped);
        space->exec_image = &image;
    }
    int result = verify(&test, &report);

    ph_number("exec phase", report.phase);
    ph_number("exec line", report.line);
    ph_number("exec result", report.result);
    ph_number("exec program status", report.program_status);
    ph_number("exec entered", report.entered);
    ph_number("exec exited", report.exited);
    ph_number("exec reclaimed", report.reclaimed);
    ph_number("exec CPUs", report.cpu_mask);
    ph_number("exec user failure line", report.user_failure.line);
    ph_number("exec user failure error", report.user_failure.error);
    ph_number("exec warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.entered == 2 && report.exited == 2 && report.reclaimed == 2);
    PH_CHECK(report.cpu_mask == 3 && !report.warnings && !report.program_status);
    for (unsigned index = 0; index < 2; ++index) {
        detach_image(vm->spaces[index]);
    }
    ph_exec_release_image(&image);
}
