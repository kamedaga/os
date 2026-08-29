#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "drmd/ipc_protocol.h"
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
    failures += expect(sizeof(netd_listen_t) == 16, "netd listen request size");
    failures += expect(sizeof(netd_poll_t) == 64, "netd poll request size");
    failures += expect(sizeof(netd_io_t) <= NETD_PAGE_BYTES, "netd io fits page");
    failures += expect(sizeof(termd_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "termd io fits page");
    failures += expect(sizeof(drmd_ioctl_request_t) <= PACHA_SERVICE_PAGE_BYTES, "drmd ioctl fits page");
    failures += expect(sizeof(drmd_read_request_t) <= PACHA_SERVICE_PAGE_BYTES, "drmd read fits page");
    failures += expect(sizeof(lpr_client_path_request_t) == 496, "lpr client path size");
    failures += expect(
        LPR_DRMD_DRM_ENDPOINT_FD == 243 && LPR_INPUTD_INPUT_ENDPOINT_FD == 244 &&
        LPR_BOOTSTRAP_FD == 245 &&
        LPR_SUPERVISOR_ENDPOINT_FD == 246 && LPRS_BOOT_CONFIG_FD == 247,
        "lpr fixed service and bootstrap fds are distinct and contiguous");
    failures += expect(
        LPRS_OP_HELLO == 0 && LPRS_OP_PROCESS_REGISTER_EXEC == 1 &&
        LPRS_OP_PROCESS_GET_STATE == 3 && LPRS_OP_PROCESS_LIST == 4 &&
        LPRS_OP_PROCESS_GETSID == 15 &&
        LPRS_OP_PROCESS_SET_PDEATHSIG == 16 &&
        LPRS_OP_PROCESS_GET_PDEATHSIG == 17 &&
        LPRS_OP_SIGNAL_KILL == 18 && LPRS_OP_CWD_GET == 20 &&
        LPRS_OP_DIAG_ERROR_GET == 23,
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
        NETD_OP_SOCKET == 2 && NETD_OP_SOCKETPAIR == 3 &&
        NETD_OP_SEND == 6 && NETD_OP_POLL == 8 && NETD_OP_BIND == 9 &&
        NETD_OP_LISTEN == 10 && NETD_OP_ACCEPT == 11 && NETD_OP_DUP == 14,
        "netd ops are contiguous from zero");
    failures += expect(
        TERMD_OP_HELLO == 0 && TERMD_OP_OPEN_PTMX == 1 &&
        TERMD_OP_HANDLE_CLOSE == 5 && TERMD_OP_HANDLE_READ == 7 &&
        TERMD_OP_SIGNAL_TAKE == 11 && TERMD_OP_DIAG_ERROR_GET == 14,
        "termd ops are contiguous from zero");
    failures += expect(
        DRMD_OP_HELLO == 0 && DRMD_OP_OPEN_NODE == 1 &&
        DRMD_OP_HANDLE_IOCTL == 4 && DRMD_OP_HANDLE_MMAP == 5 &&
        DRMD_OP_HANDLE_READ == 6 && DRMD_OP_HANDLE_POLL == 7 &&
        DRMD_OP_PRIME_EXPORT == 8 && DRMD_OP_PRIME_IMPORT_SYNC_FILE == 10 &&
        DRMD_OP_PRIME_ACQUIRE == 12,
        "drmd ops are contiguous from zero");
    failures += expect(
        LPR_COORD_OP_REGISTER_PROCESS == 0 && LPR_COORD_OP_SHARE_FD_TABLE == 7,
        "coordinator ops are contiguous from zero");

    return failures == 0 ? 0 : 1;
}
