#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "gpud/drm_protocol.h"
#include "koboxd/control_protocol.h"
#include "koboxd/storage_protocol.h"
#include "ipc_service.h"
#include "lpr_supervisor/ipc_protocol.h"
#include "lpr_supervisor/boot_config.h"
#include "netd/ipc_protocol.h"
#include "pacha/service_abi.h"
#include "personality/coordinator_protocol.h"
#include "personality/lpr_client_abi.h"
#include "personality/lpr_image_abi.h"
#include "termd/ipc_protocol.h"
#include "unixd/ipc_protocol.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pachaos/abi.h"

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "userland service abi layout failed: %s\n", message);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += expect(sizeof(lprs_register_exec_t) == 992 &&
        offsetof(lprs_register_exec_t, account) == 920 &&
        sizeof(lprs_credential_request_t) == 312,
        "launch account and explicit rights; authenticated credential operation");

    failures += expect(sizeof(lprs_credentials_t) == 288 &&
        offsetof(lprs_process_state_t, credentials) == 56 &&
        offsetof(lprs_credentials_t, groups) == 32 &&
        offsetof(lprs_process_state_t, filed_rights) == 344 &&
        offsetof(lprs_process_state_t, foreground_pgrp) == 352 &&
        sizeof(lprs_process_state_t) == 920,
        "supervisor state includes authoritative real/effective/saved UID and GID");

    failures += expect(sizeof(struct pacha_process_fd_grant) == 32 &&
        offsetof(struct pacha_process_fd_grant, source_fd) == 0 &&
        offsetof(struct pacha_process_fd_grant, target_fd) == 8 &&
        offsetof(struct pacha_process_fd_grant, rights) == 16 &&
        offsetof(struct pacha_process_fd_grant, flags) == 24,
        "PROCESS_CREATE explicit grant layout");
    failures += expect(sizeof(filed_exec_fd_grant_t) == 24 &&
        offsetof(filed_exec_fd_grant_t, rights) == 8 &&
        offsetof(filed_exec_fd_grant_t, flags) == 16,
        "filed exec separates child grant authority from IPC transport rights");

    failures += expect(PACHA_THREAD_SELF_FD == PACHAOS_THREAD_SELF_FD &&
        PACHA_THREAD_SELF_FD != PACHA_PROCESS_SELF_FD, "thread observation pseudo fd");
    failures += expect(sizeof(struct unix_attachment) == 80, "unix socket attachment size");
    failures += expect(UNIX_SERVICE_VERSION == 9, "unix control version");
    failures += expect(sizeof(struct unix_control) == 1352, "unix control size");
    failures += expect(offsetof(struct unix_control, credentials) == 64,
        "unix control header size");
    failures += expect(offsetof(struct unix_control, diagnostic) == 1184,
        "unix diagnostic offset");
    failures += expect(sizeof(struct unix_socket_diagnostic) == 168,
        "unix diagnostic size");
    failures += expect(sizeof(struct unix_control) <= UNIX_CONTROL_BYTES,
        "unix control fits page");
    failures += expect(UNIX_RIGHTS_MAX == 253, "unix ancillary limit");
    failures += expect(sizeof(struct lprs_boot_config) == 128 &&
        offsetof(struct lprs_boot_config, unix_admin_fd) == 16 &&
        offsetof(struct lprs_boot_config, filed_admin_fd) == 24 &&
        offsetof(struct lprs_boot_config, flags) == 32, "supervisor service admin bootstrap");

    failures += expect(sizeof(pacha_service_envelope_t) == 64, "service envelope size");
    failures += expect(
        sizeof(filed_service_endpoint_request_t) == 32,
        "filed service endpoint size");
    failures += expect(sizeof(filed_path_request_t) == 512, "filed path request size");
    failures += expect(sizeof(filed_file_vmo_request_t) == 48, "filed file-vmo request size");
    failures += expect(sizeof(filed_statx_t) == 128, "filed statx reply size");
    failures += expect(
        offsetof(filed_statx_t, inode_number) == 8,
        "filed statx inode identity offset");
    failures += expect(sizeof(filed_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "filed io fits page");
    failures += expect(sizeof(filed_openat_t) <= FILED_PAGE_BYTES, "filed openat payload fits page");
    failures += expect(
        FILED_RIGHT_LOOKUP == (1u << 0) && FILED_RIGHT_READ == (1u << 1) &&
        FILED_RIGHT_WRITE == (1u << 2) && FILED_RIGHT_EXEC == (1u << 3) &&
        FILED_RIGHT_STAT == (1u << 4) && FILED_RIGHT_SETATTR == (1u << 5) &&
        FILED_RIGHT_GETDENTS == (1u << 6) && FILED_RIGHT_CREATE == (1u << 7) &&
        FILED_RIGHT_REMOVE == (1u << 8) && FILED_RIGHT_RENAME == (1u << 9),
        "filed rights ABI remains contiguous through rename");
    failures += expect(
        sizeof(filed_symlink_t) <= FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
        "filed symlink payload fits after service header");
    failures += expect(
        sizeof(filed_readlink_t) <= FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
        "filed readlink payload fits after service header");
    failures += expect(
        sizeof(filed_exec_path_t) <= FILED_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
        "filed exec payload fits after service header");
    failures += expect(
        (unsigned int)FILED_FAST_VERSION == (unsigned int)PACHA_SERVICE_ABI_VERSION,
        "filed fast version follows service abi");
    failures += expect(
        sizeof(filed_fast_request_t) * FILED_FAST_REQUEST_CAPACITY <= FILED_FAST_PAYLOAD_OFFSET,
        "filed fast request ring fits pre-payload area");
    failures += expect(sizeof(storage_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "storage io fits page");
    failures += expect(
        KOBOXD_CONTROL_GET_ENDPOINT == 0 && KOBOXD_BLOCK_IDENTIFY == 0,
        "kobox endpoint-local ops start at zero");
    failures += expect(
        KOBOXD_IPC_CONTROL_OP_HELLO == 0 && KOBOXD_IPC_CONTROL_OP_DEBUG_DUMP == 5 &&
        KOBOXD_IPC_BLOCK_OP_IDENTIFY == 0 && KOBOXD_IPC_BLOCK_OP_FLUSH == 3 &&
        KOBOXD_IPC_FS_OP_MOUNT_ROOT == 0 && KOBOXD_IPC_FS_OP_RENAME == 10 &&
        KOBOXD_IPC_EVENT_OP_SUBSCRIBE == 0 && KOBOXD_IPC_EVENT_OP_NEXT == 2,
        "kobox extended endpoint ops are contiguous from zero");
    failures += expect(sizeof(koboxd_ipc_header_t) == 96, "kobox request header size");
    failures += expect(sizeof(koboxd_ipc_reply_header_t) == 80, "kobox reply header size");
    failures += expect(KOBOXD_ENDPOINT_FS_BACKEND != KOBOXD_ENDPOINT_FILED, "kobox endpoint ids distinct");
    failures += expect(sizeof(netd_socket_t) == 64, "netd socket request size");
    failures += expect(sizeof(netd_connect_t) == 64, "netd connect request size");
    failures += expect(sizeof(netd_poll_t) == 64, "netd poll request size");
    failures += expect(sizeof(netd_io_t) <= NETD_PAGE_BYTES, "netd io fits page");
    failures += expect(sizeof(termd_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "termd io fits page");
    failures += expect(sizeof(gpud_drm_ioctl_request_t) <= PACHA_SERVICE_PAGE_BYTES, "gpud ioctl fits page");
    failures += expect(sizeof(gpud_drm_read_request_t) <= PACHA_SERVICE_PAGE_BYTES, "gpud read fits page");
    failures += expect(sizeof(lpr_client_path_request_t) == 496, "lpr client path size");
    failures += expect(
        LPR_GPUD_DRM_ENDPOINT_FD == 243 && LPR_INPUTD_INPUT_ENDPOINT_FD == 244 &&
        LPR_BOOTSTRAP_FD == 245 &&
        LPR_SUPERVISOR_ENDPOINT_FD == 246 && LPRS_BOOT_CONFIG_FD == 247,
        "lpr fixed service and bootstrap fds are distinct and contiguous");
    failures += expect(
        LPRS_OP_HELLO == 0 && LPRS_OP_PROCESS_REGISTER_EXEC == 1 &&
        LPRS_OP_PROCESS_ACTIVATE == 3 &&
        LPRS_OP_PROCESS_UNIX_SESSION == 4 && LPRS_OP_PROCESS_FILED_SESSION == 5 &&
        LPRS_OP_PROCESS_GET_STATE == 6 && LPRS_OP_PROCESS_CREDENTIALS == 7 && LPRS_OP_PROCESS_LIST == 8 &&
        LPRS_OP_PROCESS_EXEC_PREPARE == 13 &&
        LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN == 14 &&
        LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL == 15 &&
        LPRS_OP_PROCESS_EXEC_COMMIT_DONE == 16 &&
        LPRS_OP_PROCESS_SET_COMM == 17 && LPRS_OP_PROCESS_QUERY == 18 &&
        LPRS_OP_PROCESS_DIAG_ATTACH == 19 && LPRS_OP_PROCESS_WAIT4 == 20 &&
        LPRS_OP_PROCESS_GETSID == 24 &&
        LPRS_OP_PROCESS_SET_PDEATHSIG == 25 &&
        LPRS_OP_PROCESS_GET_PDEATHSIG == 26 &&
        LPRS_OP_SIGNAL_KILL == 27 && LPRS_OP_CWD_GET == 29 &&
        LPRS_OP_DIAG_ERROR_GET == 32,
        "lpr supervisor process, signal, cwd, and diagnostic ops are contiguous");
    failures += expect(
        sizeof(lprs_process_list_t) == LPRS_PAYLOAD_BYTES,
        "lpr supervisor process list fills payload");
    failures += expect(
        sizeof(lprs_pdeathsig_t) == 24,
        "lpr supervisor parent-death signal payload size");

    pacha_service_envelope_t header;
    memset(&header, 0, sizeof(header));
    header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    header.abi_version = PACHA_SERVICE_ABI_VERSION;
    header.service_id = PACHA_SERVICE_ID_FILED;
    header.payload_size = 32;
    failures += expect(
        pacha_service_request_is_valid(&header, PACHA_SERVICE_ID_FILED),
        "valid filed request accepted");
    header.abi_version = 1;
    failures += expect(
        !pacha_service_request_is_valid(&header, PACHA_SERVICE_ID_FILED),
        "previous header rejected");

    failures += expect(
        STORAGE_OP_HELLO == 0 && STORAGE_OP_MOUNT_ROOT == 1 &&
        STORAGE_OP_LOOKUP == 2 && STORAGE_OP_STATFS == 4 &&
        STORAGE_OP_PREAD == 6 && STORAGE_OP_CREATE == 9 &&
        STORAGE_OP_LINK == 13 && STORAGE_OP_MKNOD == 17 &&
        STORAGE_OP_RELEASE_OBJECT == 19 && STORAGE_OP_DIAG_DUMP == 21,
        "storage ops are contiguous from zero");
    failures += expect(sizeof(storage_link_request_t) == 112, "storage link request size");
    failures += expect(sizeof(storage_statx_reply_t) == 112, "storage statx reply size");
    failures += expect(sizeof(storage_statfs_reply_t) == 120, "storage statfs reply size");
    failures += expect(
        offsetof(storage_statx_reply_t, inode_number) == 8,
        "storage statx inode identity offset");
    failures += expect(
        NETD_OP_HELLO == 0 && NETD_OP_PAGE_ATTACH == 1 &&
        NETD_OP_SOCKET == 2 && NETD_OP_CONNECT == 3 && NETD_OP_CLOSE == 4 &&
        NETD_OP_SEND == 5 && NETD_OP_RECV == 6 && NETD_OP_POLL == 7 && NETD_OP_BIND == 8 &&
        NETD_OP_UEVENT_PUBLISH == 9 && NETD_OP_DUP == 10,
        "netd ops are contiguous from zero");
    failures += expect(
        TERMD_OP_HELLO == 0 && TERMD_OP_OPEN_PTMX == 1 &&
        TERMD_OP_HANDLE_CLOSE == 5 && TERMD_OP_HANDLE_READ == 7 &&
        TERMD_OP_SIGNAL_TAKE == 11 && TERMD_OP_DIAG_ERROR_GET == 14,
        "termd ops are contiguous from zero");
    failures += expect(
        GPUD_DRM_OP_HELLO == 0 && GPUD_DRM_OP_OPEN_NODE == 1 &&
        GPUD_DRM_OP_HANDLE_IOCTL == 4 && GPUD_DRM_OP_HANDLE_MMAP == 5 &&
        GPUD_DRM_OP_HANDLE_READ == 6 && GPUD_DRM_OP_HANDLE_POLL == 7 &&
        GPUD_DRM_OP_PRIME_EXPORT == 8 && GPUD_DRM_OP_PRIME_IMPORT_SYNC_FILE == 10 &&
        GPUD_DRM_OP_PRIME_ACQUIRE == 12,
        "gpud ops are contiguous from zero");
    failures += expect(
        LPR_COORD_OP_REGISTER_PROCESS == 0 && LPR_COORD_OP_SHARE_FD_TABLE == 7,
        "coordinator ops are contiguous from zero");

    return failures == 0 ? 0 : 1;
}
