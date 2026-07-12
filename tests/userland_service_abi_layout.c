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
    failures += expect(sizeof(filed_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "filed io fits page");
    failures += expect(sizeof(filed_openat_t) <= FILED_PAGE_BYTES, "filed openat payload fits page");
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
    failures += expect(sizeof(drmd_ioctl_request_t) <= PACHA_SERVICE_PAGE_BYTES, "drmd ioctl fits page");
    failures += expect(sizeof(drmd_read_request_t) <= PACHA_SERVICE_PAGE_BYTES, "drmd read fits page");
    failures += expect(sizeof(lpr_client_path_request_t) == 496, "lpr client path size");
    failures += expect(
        LPR_DRMD_DRM_ENDPOINT_FD == 243 && LPR_INPUTD_INPUT_ENDPOINT_FD == 244 &&
        LPR_BOOTSTRAP_FD == 245 &&
        LPR_SUPERVISOR_ENDPOINT_FD == 246 && LPRS_BOOT_CONFIG_FD == 247,
        "lpr fixed service and bootstrap fds are distinct and contiguous");

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
        FILED_OP_HELLO == 0 && FILED_OP_SESSION_OPEN == 1 &&
        FILED_OP_VFS_ROOT_STAT == 4 && FILED_OP_VFS_MKNOD == 26 &&
        FILED_OP_EXEC_PATH == 38 && FILED_OP_SERVICE_SET_NETD_SOCKET == 40 &&
        FILED_OP_SERVICE_SET_DRMD_DRM == 42 && FILED_OP_SERVICE_SET_INPUTD_INPUT == 43 &&
        FILED_OP_DIAG_PING == 45 && FILED_OP_DIAG_SET_CACHE_SLOTS == 49,
        "filed ops are contiguous from zero");
    failures += expect(
        STORAGE_OP_HELLO == 0 && STORAGE_OP_MOUNT_ROOT == 1 &&
        STORAGE_OP_LOOKUP == 2 && STORAGE_OP_PREAD == 5 &&
        STORAGE_OP_CREATE == 8 && STORAGE_OP_MKNOD == 15 &&
        STORAGE_OP_RELEASE_OBJECT == 17 && STORAGE_OP_DIAG_DUMP == 19,
        "storage ops are contiguous from zero");
    failures += expect(
        NETD_OP_HELLO == 0 && NETD_OP_SOCKET == 1 && NETD_OP_SEND == 4 &&
        NETD_OP_POLL == 6 && NETD_OP_BIND == 7 && NETD_OP_ACCEPT == 9,
        "netd ops are contiguous from zero");
    failures += expect(
        TERMD_OP_HELLO == 0 && TERMD_OP_OPEN_PTMX == 1 &&
        TERMD_OP_HANDLE_CLOSE == 5 && TERMD_OP_HANDLE_READ == 7 &&
        TERMD_OP_SIGNAL_TAKE == 11 && TERMD_OP_DIAG_ERROR_GET == 14,
        "termd ops are contiguous from zero");
    failures += expect(
        DRMD_OP_HELLO == 0 && DRMD_OP_OPEN_CARD == 1 &&
        DRMD_OP_HANDLE_IOCTL == 4 && DRMD_OP_HANDLE_MMAP == 5 &&
        DRMD_OP_HANDLE_READ == 6 && DRMD_OP_HANDLE_POLL == 7 &&
        DRMD_OP_PRIME_EXPORT == 8 && DRMD_OP_PRIME_ACQUIRE == 11,
        "drmd ops are contiguous from zero");
    failures += expect(
        LPRS_OP_HELLO == 0 && LPRS_OP_PROCESS_REGISTER_EXEC == 1 &&
        LPRS_OP_SIGNAL_KILL == 14 && LPRS_OP_CWD_GET == 16 &&
        LPRS_OP_DIAG_ERROR_GET == 19,
        "lprs ops are contiguous from zero");
    failures += expect(
        LPR_COORD_OP_REGISTER_PROCESS == 0 && LPR_COORD_OP_SHARE_FD_TABLE == 7,
        "coordinator ops are contiguous from zero");
    failures += expect(sizeof(lprs_process_state_t) == 608, "lprs process state size");
    failures += expect(
        FILED_EXEC_LPR_FD_DEVICE == 2 &&
        FILED_EXEC_LPR_FD_DRM == 4 &&
        FILED_EXEC_LPR_FD_INPUT == 5 &&
        FILED_EXEC_LPR_FD_PIPE == 6 &&
        FILED_EXEC_LPR_FD_EVENT == 7 &&
        FILED_EXEC_LPR_FD_NATIVE == 9 &&
        FILED_EXEC_LPR_FD_DMABUF == 10 &&
        FILED_EXEC_LPR_FD_TABLE_VERSION == 6 &&
        LPR_IMAGE_ABI_VERSION == 9 &&
        LPR_CLIENT_FD_KIND_DRMD_HANDLE == 3 &&
        LPR_CLIENT_FD_KIND_INPUTD_HANDLE == 4 &&
        LPR_CLIENT_FD_KIND_PIPE == 5 &&
        LPR_CLIENT_FD_KIND_EVENT == 6,
        "lpr fd kind order keeps device before service handles");

    return failures == 0 ? 0 : 1;
}
