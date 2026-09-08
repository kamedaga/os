#pragma once

/* Per private waiter receive channel; enforced by the service, not trusted
 * to client-side cache discipline. Duplicate registration is idempotent. */
#define UNIX_WAIT_REGISTRATION_LIMIT 256u

#include "unixd/transport.h"
#include "unixd/transfer.h"
#include "unixd/poll.h"

#define UNIX_SERVICE_MAGIC UINT64_C(0x3151455258494e55)
#define UNIX_REPLY_MAGIC UINT64_C(0x3159505258494e55)
#define UNIX_NOTIFY_MAGIC UINT64_C(0x315946544e495855)
#define UNIX_SERVICE_VERSION 9u
/* This reply guarantees that dispatch did not run. Retry the same request
 * once with its VMO attached. word1=version, word2=missing token. */
#define UNIX_BUFFER_MISS_MAGIC UINT64_C(0x3153494d58494e55)
#define UNIX_CONTROL_BYTES 8192u
#define UNIX_PATH_BYTES 108u
#define UNIX_RIGHTS_MAX 253u

/* A new service wire, not aliases for netd opcodes. Administrative requests
 * are accepted only on the bootstrap endpoint held by the supervisor. */
enum unix_operation {
    UNIX_OP_HELLO = 0,
    UNIX_OP_PROCESS_REGISTER = 1,
    UNIX_OP_PROCESS_CREDENTIALS = 2,
    UNIX_OP_THREAD_REGISTER = 3,
    UNIX_OP_SOCKET = 4,
    UNIX_OP_SOCKETPAIR = 5,
    UNIX_OP_BIND = 6,
    UNIX_OP_LISTEN = 7,
    UNIX_OP_CONNECT = 8,
    UNIX_OP_ACCEPT = 9,
    UNIX_OP_NAME = 10,
    UNIX_OP_PEERCRED = 11, /* credentials=connection-time peer identity, no VMOs */
    UNIX_OP_POLL = 12,
    UNIX_OP_PENDING = 13, /* result=next packet bytes; no capability transfer */
    UNIX_OP_ATTACH = 14,
    UNIX_OP_RETAIN = 15,
    UNIX_OP_HANDOFF = 16, /* session: prepare/confirm; grant channel: authorize target session */
    UNIX_OP_SHUTDOWN = 17,
    UNIX_OP_CLOSE = 18,
    UNIX_OP_FLAGS = 19, /* transaction: 0=query, 1=set argument; result=flags */
    UNIX_OP_OPTIONS = 20, /* transaction=bit mask to change; argument=bits; result=all */
    UNIX_OP_ERROR = 21, /* result=OFD pending errno, atomically read and clear */
    /* REGISTER: socket + transaction=thread owner, argument=0 creates a
     * waiter/channel; argument=existing ID watches another socket with it.
     * SYNC: source socket + argument=ID resolves a SEND-only notification cap.
     * NOTIFY: same authorization as SYNC, but sends through the broker when
     * the updater cannot retain another native FD (not the normal hot path).
     * REMOVE: argument=ID, socket=0 destroys it, otherwise removes that watch. */
    UNIX_OP_WAIT_REGISTER = 22,
    UNIX_OP_WAIT_SYNC = 23,
    UNIX_OP_WAIT_NOTIFY = 24,
    UNIX_OP_WAIT_REMOVE = 25,
    UNIX_OP_DGRAM_ROUTE = 26,
    UNIX_OP_DGRAM_COMMIT = 27,
    UNIX_OP_DGRAM_HEAD = 28,
    UNIX_OP_DGRAM_CONSUME = 29,
    UNIX_OP_RIGHTS_PREPARE = 30,
    UNIX_OP_RIGHTS_APPEND = 31,
    UNIX_OP_RIGHTS_COMMIT = 32,
    UNIX_OP_RIGHTS_CLAIM = 33,
    UNIX_OP_RIGHTS_FINISH = 34,
    UNIX_OP_RIGHTS_ACK = 35,
    UNIX_OP_RIGHTS_CANCEL = 36,
    UNIX_OP_DIAG = 37,
};

#define UNIX_RIGHTS_PEEK 1u
#define UNIX_RIGHTS_RECEIVING 2u /* ACK only */
#define UNIX_RIGHTS_CREDENTIALS 4u /* CLAIM: credentials captured when sent */
#define UNIX_SOCKET_REUSEADDR 1u
#define UNIX_SOCKET_KEEPALIVE 2u
#define UNIX_SOCKET_PASSCRED 4u

enum unix_address_kind {
    UNIX_ADDRESS_UNNAMED = 0,
    UNIX_ADDRESS_PATH = 1,
    UNIX_ADDRESS_ABSTRACT = 2,
};

struct unix_credentials {
    uint64_t generation;
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t reserved;
};

struct unix_address {
    uint32_t kind;
    uint32_t length;
    /* Path identity is supplied by filed, never inferred from a string.
     * Abstract addresses use exactly length bytes, including interior NULs. */
    uint64_t filesystem;
    uint64_t inode;
    uint8_t bytes[UNIX_PATH_BYTES];
    uint32_t reserved;
};

struct unix_direction_diagnostic {
    uint64_t generation;
    uint64_t published;
    uint64_t consumed;
    uint64_t writer;
    uint64_t reader;
};

/* Control metadata is broker-owned; shared cursors/owners are independent
 * atomic samples, not a transactionally consistent readiness result. */
struct unix_socket_diagnostic {
    uint64_t socket;
    uint64_t peer;
    uint64_t session;
    uint64_t references;
    uint64_t delivery;
    uint64_t operation;
    uint32_t type;
    uint32_t flags;
    uint32_t owners;
    uint32_t bound;
    uint32_t listening;
    uint32_t backlog;
    uint32_t pending;
    uint32_t datagram_count;
    uint32_t datagram_bytes;
    uint32_t reserved;
    struct unix_direction_diagnostic outgoing;
    struct unix_direction_diagnostic incoming; /* DGRAM: head delivery route */
};

struct unix_attachment {
    uint64_t socket;
    uint64_t generation;
    uint64_t peer;
    uint32_t type;
    uint32_t flags;
    uint32_t listening;
    uint32_t reserved;
    struct unix_credentials peer_credentials;
    /* Two unix_endpoint VMOs: own (RW), peer (R). Each holds its writer's
     * outgoing TX and incoming RX. Shared by dup/fork/SCM_RIGHTS. */
};

struct unix_control {
    uint64_t magic;
    uint32_t version;
    uint32_t operation;
    uint64_t request;
    uint64_t socket;
    uint64_t argument;
    uint64_t transaction;
    int64_t status;
    uint64_t result;
    struct unix_credentials credentials;
    struct unix_address address;
    /* CLAIM returns a socket as a single-item segment. Its attachment is
     * authenticated by the receive reservation, before ownership FINISH.
     * generation=0 has no direction caps (listener/unconnected/DGRAM). */
    struct unix_attachment attachment;
    struct unix_poll_sequence poll_sequence;
    /* DGRAM_ROUTE/HEAD: omit the two PRIVATE mapping caps only when this
     * hint equals the broker-selected generation. Authorization, pathname
     * resolution, FIFO and current options are still evaluated every time. */
    uint64_t cached_generation;
    /* Server-issued session-bound control-page token, never caller authority.
     * IPC word2: 0=temporary VMO, 1=register attached VMO, >=2=reuse token. */
    uint64_t buffer_token;
    struct {
        uint64_t id;
        uint64_t route;
        uint64_t generation;
        uint32_t length;
        uint32_t position;
        uint64_t operation;
        uint64_t ticket;
        struct unix_address source;
        struct unix_credentials credentials;
    } delivery;
    struct {
        uint64_t owner;
        uint64_t operation; /* sender's immutable record operation on receive */
        uint64_t before;
        uint64_t after;
        uint32_t length;
        uint32_t message_length;
    } io;
    /* PREPARE: socket, io.owner, operation, total; optional DGRAM route and
     * claimed credentials (generation=0 means use authenticated credentials).
     * APPEND: ticket, offset, count, items; capability indices address only
     * attached provider caps, excluding the control page/reply cap.
     * COMMIT: ticket + io write reservation.
     * CLAIM: ticket + io read reservation, offset/count (maximum this batch);
     * returns count/items, total, credentials, result=transport generation.
     * FINISH: ticket + io, receiver operation, count=imported prefix, PEEK.
     * ACK: ticket, io.owner, operation, RECEIVING selects the receive receipt.
     * CANCEL: ticket, after releasing any local write reservation. */
    struct {
        uint64_t ticket;
        uint64_t operation;
        uint64_t route;
        uint32_t offset;
        uint32_t count;
        uint32_t total;
        uint32_t flags;
        struct unix_transfer_item items[UNIX_TRANSFER_BATCH];
    } rights;
    struct unix_socket_diagnostic diagnostic;
};


struct unix_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t admin_endpoint;
    uint64_t filed_path_channel;
    uint64_t ready_channel;
};
_Static_assert(sizeof(struct unix_boot_config) == 40, "unixd private bootstrap wire");

#define UNIX_BOOT_MAGIC UINT64_C(0x31544f4f42584955)
#define UNIX_READY_MAGIC UINT64_C(0x3159445258494e55)

_Static_assert(sizeof(struct unix_control) <= UNIX_CONTROL_BYTES,
    "unix control page size");
_Static_assert(offsetof(struct unix_control, credentials) == 64,
    "unix control header size");
