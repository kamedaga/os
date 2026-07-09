#include <stdio.h>
#include <string.h>

#include "filed/ipc_protocol_v2.h"
#include "filed/payload_v2.h"
#include "koboxd/control_protocol_v2.h"
#include "koboxd/storage_protocol_v2.h"
#include "lpr_supervisor/ipc_protocol_v2.h"
#include "netd/ipc_protocol_v2.h"
#include "pacha/service_abi.h"
#include "personality/lpr_client_abi.h"
#include "termd/ipc_protocol_v2.h"

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "userland service abi layout failed: %s\n", message);
        return 1;
    }
    return 0;
}

static int in_range(unsigned int op, unsigned int begin, unsigned int end)
{
    return op >= begin && op <= end;
}

int main(void)
{
    int failures = 0;

    failures += expect(sizeof(pacha_service_request_header_t) == 64, "request header size");
    failures += expect(sizeof(pacha_service_reply_header_t) == 64, "reply header size");
    failures += expect(
        sizeof(filed_v2_service_endpoint_request_t) == 32,
        "filed service endpoint size");
    failures += expect(sizeof(filed_v2_path_request_t) == 512, "filed path request size");
    failures += expect(sizeof(filed_v2_file_vmo_request_t) == 48, "filed file-vmo request size");
    failures += expect(sizeof(filed_v2_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "filed io fits page");
    failures += expect(sizeof(filed_v2_openat_t) <= FILED_V2_PAGE_BYTES, "filed openat payload fits page");
    failures += expect(sizeof(filed_v2_exec_path_t) <= FILED_V2_PAGE_BYTES, "filed exec payload fits page");
    failures += expect(
        (unsigned int)FILED_V2_FAST_VERSION == (unsigned int)PACHA_SERVICE_ABI_VERSION,
        "filed fast version follows service abi");
    failures += expect(
        sizeof(filed_v2_fast_request_t) * FILED_V2_FAST_REQUEST_CAPACITY <= FILED_V2_FAST_PAYLOAD_OFFSET,
        "filed fast request ring fits pre-payload area");
    failures += expect(sizeof(storage_v2_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "storage io fits page");
    failures += expect(KOBOXD_V2_CONTROL_GET_ENDPOINT != KOBOXD_V2_BLOCK_IDENTIFY, "kobox control/block ops distinct");
    failures += expect(KOBOXD_V2_ENDPOINT_FS_BACKEND != KOBOXD_V2_ENDPOINT_FILED, "kobox endpoint ids distinct");
    failures += expect(sizeof(netd_v2_socket_t) == 64, "netd socket request size");
    failures += expect(sizeof(netd_v2_connect_t) == 64, "netd connect request size");
    failures += expect(sizeof(netd_v2_poll_t) == 64, "netd poll request size");
    failures += expect(sizeof(netd_v2_io_t) <= NETD_V2_PAGE_BYTES, "netd io fits page");
    failures += expect(sizeof(termd_v2_io_request_t) <= PACHA_SERVICE_PAGE_BYTES, "termd io fits page");
    failures += expect(sizeof(lpr_client_path_request_t) == 496, "lpr client path size");

    pacha_service_request_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    header.abi_version = PACHA_SERVICE_ABI_VERSION;
    header.service_id = PACHA_SERVICE_ID_FILED;
    header.payload_size = 32;
    failures += expect(
        pacha_service_request_header_is_v2(&header, PACHA_SERVICE_ID_FILED),
        "valid filed v2 header accepted");
    header.abi_version = 1;
    failures += expect(
        !pacha_service_request_header_is_v2(&header, PACHA_SERVICE_ID_FILED),
        "previous header rejected");

    failures += expect(
        in_range(FILED_V2_OP_VFS_OPENAT, 0x0200u, 0x02ffu) &&
        in_range(FILED_V2_OP_EXEC_PATH, 0x0300u, 0x03ffu) &&
        in_range(FILED_V2_OP_SERVICE_SET_TERMD_TTY, 0x0400u, 0x04ffu) &&
        in_range(FILED_V2_OP_DIAG_DUMP, 0x7f00u, 0x7fffu),
        "filed op ranges");
    failures += expect(
        in_range(STORAGE_V2_OP_LOOKUP, 0x0200u, 0x02ffu) &&
        in_range(STORAGE_V2_OP_PREAD, 0x0300u, 0x03ffu) &&
        in_range(STORAGE_V2_OP_RELEASE_OBJECT, 0x0500u, 0x05ffu) &&
        in_range(STORAGE_V2_OP_DIAG_DUMP, 0x7f00u, 0x7fffu),
        "storage op ranges");
    failures += expect(
        in_range(NETD_V2_OP_SOCKET, 0x0100u, 0x01ffu) &&
        in_range(NETD_V2_OP_SEND, 0x0200u, 0x02ffu) &&
        in_range(NETD_V2_OP_POLL, 0x0300u, 0x03ffu),
        "netd op ranges");
    failures += expect(
        in_range(TERMD_V2_OP_OPEN_PTMX, 0x0100u, 0x01ffu) &&
        in_range(TERMD_V2_OP_HANDLE_READ, 0x0300u, 0x03ffu) &&
        in_range(TERMD_V2_OP_SIGNAL_TAKE, 0x0400u, 0x04ffu) &&
        in_range(TERMD_V2_OP_DIAG_DUMP, 0x7f00u, 0x7fffu),
        "termd op ranges");
    failures += expect(
        in_range(LPRS_V2_OP_PROCESS_REGISTER_EXEC, 0x0100u, 0x01ffu) &&
        in_range(LPRS_V2_OP_SIGNAL_KILL, 0x0200u, 0x02ffu) &&
        in_range(LPRS_V2_OP_CWD_GET, 0x0400u, 0x04ffu) &&
        in_range(LPRS_V2_OP_DIAG_DUMP, 0x7f00u, 0x7fffu),
        "lprs op ranges");
    failures += expect(sizeof(lprs_v2_process_state_t) == 608, "lprs process state size");
    failures += expect(
        FILED_V2_EXEC_LPR_FD_PIPE == 3 &&
        FILED_V2_EXEC_LPR_FD_EVENT == 4 &&
        LPR_CLIENT_FD_KIND_PIPE == 3 &&
        LPR_CLIENT_FD_KIND_EVENT == 4,
        "lpr fd kind order keeps pipe before event");

    return failures == 0 ? 0 : 1;
}
