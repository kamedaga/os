/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic workload: repeated real VFS lifetimes after one regular boot.
 * This replaces only the chapter-2 test entry in the dedicated stress binary;
 * it must not report the full chapter-2 Gate as passed. */
#include "host.h"
#include "boot/vfs_gate.h"

void ph_verify_chapter2_core(void) {
    int (*verify)(struct kobox_linux_vfs_report *);
    void *address = ph_image_lookup(&ph_core, "kobox_linux_vfs_verify");

    PH_CHECK(address != NULL);
    memcpy(&verify, &address, sizeof(verify));
    for (unsigned round = 0; round < 64; ++round) {
        struct kobox_linux_vfs_report report = {.size = sizeof(report)};

        ph_number("VFS stress round", round + 1);
        int result = verify(&report);

        if (result) {
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
        }
        PH_OK(result);
        PH_CHECK(report.phase == 8 && report.cases == 8 && report.warnings == 0);
        PH_CHECK(report.file_reclaims == 8 && report.inode_reclaims == 8 &&
            report.folio_reclaims == 24 && report.super_reclaims == 8);
    }
    ph_log("PACHA_KOBOX_VFS_STRESS=PASS\n");
}
