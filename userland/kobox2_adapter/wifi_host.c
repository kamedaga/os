/* SPDX-License-Identifier: MIT */
#include "wifi_host.h"
#include "host.h"
#include "vm_internal.h"
#include "exec_image.h"

extern const unsigned char ph_wifi_bootstrap_start[], ph_wifi_bootstrap_end[];
extern const unsigned char ph_wifi_program_start[], ph_wifi_program_end[];

static struct ph_vm_space *space;
static struct ph_exec_image executable;
static struct kobox_linux_user_process_file files[2];
static struct kobox_linux_user_process_config process;
static const char program_path[] = "/usr/sbin/wpa_supplicant";
static const char configuration_path[] = "/etc/wpa_supplicant.conf";
static const char empty_configuration[] = "update_config=0\n";
static const char *const arguments[] = {program_path, NULL};
static const char *const environment[] = {"HOME=/", "LANG=C", NULL};

void ph_wifi_host_prepare(void) {
    PH_CHECK(!space);
    space = ph_vm_create(ph_wifi_bootstrap_start,
        (size_t)(ph_wifi_bootstrap_end - ph_wifi_bootstrap_start),
        KOBOX_X86_USER_START, KOBOX_X86_USER_END - KOBOX_X86_USER_START);
    executable = ph_exec_prepare_image(ph_wifi_program_start,
        (size_t)(ph_wifi_program_end - ph_wifi_program_start));
    space->exec_image = &executable;
    files[0] = (struct kobox_linux_user_process_file){
        .path = program_path, .data = executable.bytes,
        .size = executable.length, .mode = 0755};
    files[1] = (struct kobox_linux_user_process_file){
        .path = configuration_path, .data = empty_configuration,
        .size = sizeof(empty_configuration) - 1, .mode = 0600};
    process = (struct kobox_linux_user_process_config){.size = sizeof(process),
        .vm_operations = &ph_vm_ops, .host_space = space,
        .start = KOBOX_X86_USER_START,
        .length = KOBOX_X86_USER_END - KOBOX_X86_USER_START,
        .files = files, .file_count = 2, .argv = arguments, .envp = environment,
        .capability_mask = (UINT64_C(1) << 12) | (UINT64_C(1) << 13)};
}

const struct kobox_linux_user_process_config *ph_wifi_host_config(void) {
    PH_CHECK(space);
    return &process;
}

void ph_wifi_host_finish(void) {
    PH_CHECK(space);
    ph_vm_destroy(space);
    space = NULL;
    ph_exec_release_image(&executable);
    memset(&process, 0, sizeof(process));
    memset(files, 0, sizeof(files));
}
