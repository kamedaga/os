/* SPDX-License-Identifier: MIT */
/* The fixed Linux core loads native .ko files and owns all GEM semantics. */
#include "host.h"
#include "bootfs.h"
#include "vm.h"
#include "boot/module_gate.h"

extern int ph_module_access(void *address, enum kobox_linux_module_access operation);

void ph_verify_gem_gate(const struct kobox_linux_vm_test *vm) {
    static const char *const modules[KOBOX_GEM_MODULES] = {
        "/srv/kobox2/modules/i2c-core.ko",
        "/srv/kobox2/modules/drm_panel_orientation_quirks.ko",
        "/srv/kobox2/modules/drm.ko",
        "/srv/kobox2/modules/drm_shmem_helper.ko",
    };
    struct kobox_linux_module_test test = {
        .size = sizeof(test), .vm = vm, .access = ph_module_access,
        .gem_final_owner = KOBOX_GEM_FINAL_VMA,
    };
    struct kobox_linux_module_report *report = ph_alloc(sizeof(*report));
    int (*verify)(const struct kobox_linux_module_test *, struct kobox_linux_module_report *);

    report->size = sizeof(*report);
    for (unsigned index = 0; index < KOBOX_GEM_MODULES; ++index) {
        test.images[index].data = ph_bootfs_file(modules[index], &test.images[index].length);
    }
    test.lifetime_image.data = ph_bootfs_file("/srv/kobox2/modules/lifetime_test.ko",
        &test.lifetime_image.length);
    void *address = ph_image_lookup(&ph_core, "kobox_linux_module_probe");

    PH_CHECK(address != NULL);
    memcpy(&verify, &address, sizeof(verify));
    int result = verify(&test, report);

    ph_number("module phase", report->phase);
    ph_number("module result", report->result);
    ph_number("module loaded", report->loaded);
    ph_number("module unloaded", report->unloaded);
    ph_number("module permissions", report->permissions);
    ph_number("GEM phase", report->gem.phase);
    ph_number("GEM line", report->gem.line);
    ph_number("GEM result", report->gem.result);
    ph_number("GEM files", report->gem.files);
    ph_number("GEM handles", report->gem.handles);
    ph_number("GEM mappings", report->gem.mappings);
    ph_number("GEM accesses", report->gem.accesses);
    ph_number("GEM faults", report->gem.faults);
    ph_number("GEM denied", report->gem.denied);
    ph_number("GEM objects reclaimed", report->gem.object_reclaims);
    ph_number("GEM pages reclaimed", report->gem.page_reclaims);
    ph_number("module warnings", report->warnings);
    if (report->diagnostics[0]) {
        report->diagnostics[sizeof(report->diagnostics) - 1] = 0;
        ph_log(report->diagnostics);
    }
    PH_OK(result);
    PH_CHECK(!report->result && !report->warnings);
    PH_CHECK(report->loaded == KOBOX_GEM_MODULES + 1 && report->unloaded == report->loaded);
    ph_free(report, sizeof(*report));
}
