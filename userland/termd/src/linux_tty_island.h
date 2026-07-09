#ifndef TERMD_LINUX_TTY_ISLAND_H
#define TERMD_LINUX_TTY_ISLAND_H

#include "termd/boot_config.h"
#include "termd/ipc_protocol_v2.h"

#include <stdint.h>

#include <kobox/device.h>
#include <kobox/module.h>
#include <kobox/platform.h>

struct termd_linux_tty_island {
    uint8_t ready;
    uint8_t reserved0[3];
    uint32_t source_count;
    uint32_t compiled_source_count;
    uint32_t configured_module_count;
    uint32_t loaded_module_count;
    void *devpts_root;
    int32_t devpts_status;
    uint64_t ptmx_dev;
    void *ptmx_cdev;
    void *ptmx_fops;
    void *ptmx_open;
    void *ptmx_ioctl;
    void *ptmx_poll;
    void *ptmx_read_iter;
    void *ptmx_write_iter;
    void *ptmx_release;
    kb_module_t *ptmx_owner;
    int ptmx_registered;
    int32_t load_status;
    int32_t init_status;
    uint64_t signal_generation;
    const char *loader_version;
    kb_device_backend_t *backend;
    kb_platform_t *platform;
    kb_module_t *modules[TERMD_MAX_MODULES];
};

int termd_linux_tty_island_init(
    struct termd_linux_tty_island *island,
    const struct termd_boot_config *cfg);

int termd_linux_tty_island_refresh_ptmx(struct termd_linux_tty_island *island);
int termd_linux_tty_island_open_ptmx(
    struct termd_linux_tty_island *island,
    uint64_t flags,
    uint64_t *out_handle);
int termd_linux_tty_island_open_pts(
    struct termd_linux_tty_island *island,
    const termd_v2_open_request_t *request,
    uint64_t *out_handle);
int termd_linux_tty_island_open_hvc(
    struct termd_linux_tty_island *island,
    const termd_v2_open_request_t *request,
    uint64_t *out_handle);
int termd_linux_tty_island_open_ctty(
    struct termd_linux_tty_island *island,
    const termd_v2_open_request_t *request,
    uint64_t *out_handle);
int termd_linux_tty_island_close(struct termd_linux_tty_island *island, uint64_t handle);
int termd_linux_tty_island_dup(
    struct termd_linux_tty_island *island,
    uint64_t handle,
    uint64_t *out_handle);
int termd_linux_tty_island_ioctl(
    struct termd_linux_tty_island *island,
    termd_v2_ioctl_request_t *request);
int termd_linux_tty_island_poll(
    struct termd_linux_tty_island *island,
    termd_v2_poll_request_t *request);
void termd_linux_tty_island_pump(struct termd_linux_tty_island *island);
int termd_linux_tty_island_io(
    struct termd_linux_tty_island *island,
    int write,
    termd_v2_io_request_t *request,
    uint64_t *out_result);
int termd_linux_tty_island_take_signal(
    struct termd_linux_tty_island *island,
    termd_v2_signal_request_t *request,
    uint64_t *out_result);

#endif
