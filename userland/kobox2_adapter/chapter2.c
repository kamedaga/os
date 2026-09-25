/* SPDX-License-Identifier: MIT */
/* Core-side chapter 2 prerequisites. External VM/client Gates run separately. */
#include "host.h"
#include "boot/vfs_gate.h"
#include "boot/shmem_gate.h"
#include "boot/pressure_gate.h"
#include "boot/client_task_gate.h"

#define RESOLVE_VERIFY(function, symbol) do { \
    void *address = ph_image_lookup(&ph_core, symbol); \
    PH_CHECK(address != NULL); \
    memcpy(&(function), &address, sizeof(function)); \
} while (0)

static void run_vfs_gate(void) {
    int (*verify)(struct kobox_linux_vfs_report *);
    struct kobox_linux_vfs_report report = {.size = sizeof(report)};

    RESOLVE_VERIFY(verify, "kobox_linux_vfs_verify");
    int result = verify(&report);

    ph_number("VFS phase", report.phase);
    ph_number("VFS line", report.line);
    ph_number("VFS result", report.result);
    ph_number("VFS CPU", report.cpu);
    ph_number("VFS deferred", report.deferred);
    ph_number("VFS unlink first", report.unlink_first);
    ph_number("VFS cases", report.cases);
    ph_number("VFS files reclaimed", report.file_reclaims);
    ph_number("VFS inodes reclaimed", report.inode_reclaims);
    ph_number("VFS folios reclaimed", report.folio_reclaims);
    ph_number("VFS superblocks reclaimed", report.super_reclaims);
    ph_number("VFS warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_VFS=PASS\n");
}

static void run_shmem_gate(void) {
    int (*verify)(struct kobox_linux_shmem_report *);
    struct kobox_linux_shmem_report report = {.size = sizeof(report)};

    RESOLVE_VERIFY(verify, "kobox_linux_shmem_verify");
    int result = verify(&report);

    ph_number("shmem phase", report.phase);
    ph_number("shmem line", report.line);
    ph_number("shmem cases", report.cases);
    ph_number("shmem sharing checks", report.sharing_checks);
    ph_number("shmem race rounds", report.race_rounds);
    ph_number("shmem pages reclaimed", report.page_reclaims);
    ph_number("shmem rollbacks", report.rollbacks);
    ph_number("shmem warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_SHMEM=PASS\n");
}

static void run_pressure_gate(bool allocation_failures) {
    int (*verify)(struct kobox_linux_pressure_report *);
    struct kobox_linux_pressure_report report = {.size = sizeof(report)};
    const char *symbol = allocation_failures ?
        "kobox_linux_allocation_verify" : "kobox_linux_pressure_verify";

    RESOLVE_VERIFY(verify, symbol);
    int result = verify(&report);

    ph_log(allocation_failures ? "allocation failure Gate\n" : "RAM pressure Gate\n");
    ph_number("pressure phase", report.phase);
    ph_number("pressure line", report.line);
    ph_number("pressure pages", report.pressure_pages);
    ph_number("pressure kswapd freed", report.kswapd_freed);
    ph_number("pressure direct freed", report.direct_freed);
    ph_number("pressure cache reused", report.cache_reused);
    ph_number("pressure allocation case", report.allocation_case);
    ph_number("pressure fail nth", report.fail_nth);
    ph_number("pressure injected", report.injected);
    ph_number("pressure rollbacks", report.rollbacks);
    ph_number("pressure sweeps", report.sweeps);
    ph_number("pressure warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log(allocation_failures ? "PACHA_KOBOX_ALLOCATION=PASS\n" : "PACHA_KOBOX_PRESSURE=PASS\n");
}

static void run_client_task_gate(void) {
    int (*verify)(struct kobox_client_task_report *);
    struct kobox_client_task_report report = {.size = sizeof(report)};

    RESOLVE_VERIFY(verify, "kobox_linux_client_task_verify");
    int result = verify(&report);

    ph_number("client task phase", report.phase);
    ph_number("client task line", report.line);
    ph_number("client tasks", report.tasks);
    ph_number("client task mappings", report.mappings);
    ph_number("client task files", report.files);
    ph_number("client tasks reaped", report.reaped);
    ph_number("client task warnings", report.warnings);
    PH_OK(result);
    PH_CHECK(report.warnings == 0);
    ph_log("PACHA_KOBOX_CLIENT_TASK=PASS\n");
}

void ph_verify_chapter2_core(void) {
    /* No fixture-side initcalls or subsystem initialization. Each verifier
     * uses the same already-booted Linux core and upstream lifetime paths. */
    run_vfs_gate();
    run_shmem_gate();
    run_pressure_gate(false);
    run_pressure_gate(true);
    run_client_task_gate();
    ph_log("PACHA_KOBOX_CHAPTER2_CORE=PASS\n");
}
