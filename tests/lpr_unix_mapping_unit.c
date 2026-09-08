#define _GNU_SOURCE
#include "../userland/personality/linux/runtime/lpr_unix/mapping.c"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Host memfds stand in for native VMOs; mappings have real RO/RW protection.
 * Control IPC and native descriptor installation are mocked, not guest-tested. */
static int backing[2];
static struct unix_endpoint *broker_pages[2];
static uint64_t sizes[2] = { sizeof(struct unix_endpoint), sizeof(struct unix_endpoint) };
static struct { unsigned live, backing; uint64_t rights; } caps[256];
static struct { void *address; uint64_t bytes; } mappings[8];
static unsigned rpc_calls, map_calls, active_maps;
static int fail_map = -1;
enum { NORMAL, EXTRA_WRITE, NO_READ, WRONG_SIZE, WRONG_KIND, PRIVATE_CAP,
    WRONG_TYPE, STALE_GENERATION, BAD_HEADER, SHORT_REPLY, DUPLICATE_CAP };
static unsigned mode;
static unsigned peer_gone;
static uint32_t socket_type;

static void initialize(uint32_t type)
{
    socket_type = type;
    for (unsigned i = 0; i < 2; i++) memset(broker_pages[i], 0, sizes[i]);
    assert(unix_transport_init(&broker_pages[0]->tx, &broker_pages[1]->rx, 7, type) == 0);
    assert(unix_transport_init(&broker_pages[1]->tx, &broker_pages[0]->rx, 7, type) == 0);
}

int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(client && request->operation == UNIX_OP_ATTACH && request->request);
    assert(!send && !send_count && capacity == 2 && (request->socket == 10 || request->socket == 20));
    rpc_calls++;
    const unsigned side = request->socket == 20;
    request->result = mode == STALE_GENERATION ? 8 : 7;
    request->argument = peer_gone ? 0 : side ? 10 : 20;
    request->transaction = mode == WRONG_TYPE ? UNIX_TRANSPORT_DGRAM : socket_type;
    request->credentials = (struct unix_credentials){ .pid = side ? 11 : 22, .generation = 2 };
    *received = mode == SHORT_REPLY ? 1 : 2;
    for (unsigned i = 0; i < *received; i++) {
        const unsigned fd = 20 + side * 20 + (mode == DUPLICATE_CAP && i == 1 ? 0 : i);
        if (!(mode == DUPLICATE_CAP && i == 1)) {
            assert(!caps[fd].live);
            caps[fd].live = 1; caps[fd].backing = (i + side) % 2;
            caps[fd].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ |
                (i == 0 ? PACHA_FD_RIGHT_MAP_WRITE : 0);
            if (mode == EXTRA_WRITE && i == 1) caps[fd].rights |= PACHA_FD_RIGHT_MAP_WRITE;
            if (mode == NO_READ && i == 1) caps[fd].rights &= ~PACHA_FD_RIGHT_MAP_READ;
        }
        receive[i] = (struct pacha_ipc_fd){ .fd = fd };
    }
    if (mode == BAD_HEADER) broker_pages[1]->tx.capacity++;
    return 0;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd < 256 && caps[fd].live);
    caps[fd].live = 0;
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) {
        assert(a0 < 256 && caps[a0].live);
        struct pacha_fd_info *info = (struct pacha_fd_info *)(uintptr_t)a1;
        *info = (struct pacha_fd_info){ .kind = mode == WRONG_KIND ? PACHA_FD_KIND_CHANNEL : PACHA_FD_KIND_VMO,
            .rights = caps[a0].rights, .size = sizes[caps[a0].backing] + (mode == WRONG_SIZE ? 4096 : 0),
            .flags = sizes[caps[a0].backing] != sizeof(struct unix_endpoint) ?
                PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC : mode == PRIVATE_CAP ? PACHA_FD_FLAG_PRIVATE : 0 };
        return 0;
    }
    assert(nr == PACHAOS_SYSCALL_MUNMAP);
    unsigned i;
    for (i = 0; i < 8 && mappings[i].address != (void *)(uintptr_t)a0; i++) {}
    assert(i < 8 && mappings[i].bytes == a1 && active_maps);
    assert(munmap(mappings[i].address, mappings[i].bytes) == 0);
    mappings[i].address = NULL; active_maps--;
    return 0;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address, uint64_t bytes,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd < 256 && caps[fd].live && !address && !offset);
    assert(bytes == sizes[caps[fd].backing] && flags == PACHA_MMAP_SHARED);
    assert(prot & PACHA_PROT_READ);
    assert(!(prot & PACHA_PROT_WRITE) || (caps[fd].rights & PACHA_FD_RIGHT_MAP_WRITE));
    if ((int)map_calls++ == fail_map) return 4;
    unsigned slot;
    for (slot = 0; slot < 8 && mappings[slot].address; slot++) {}
    assert(slot < 8);
    void *mapped = mmap(NULL, bytes, PROT_READ | ((prot & PACHA_PROT_WRITE) ? PROT_WRITE : 0),
        MAP_SHARED, backing[caps[fd].backing], 0);
    assert(mapped != MAP_FAILED);
    mappings[slot].address = mapped; mappings[slot].bytes = bytes; active_maps++;
    return (int64_t)(uintptr_t)mapped;
}

static void expect_readonly(const void *address)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL);
        *(volatile unsigned char *)address ^= 1;
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}

static void copy_read(const struct unix_read *read, char *out)
{
    memcpy(out, read->spans[0].base, read->spans[0].length);
    if (read->spans[1].length) memcpy(out + read->spans[0].length, read->spans[1].base, read->spans[1].length);
}

int main(void)
{
    for (unsigned i = 0; i < 2; i++) {
        backing[i] = memfd_create("lpr-unix-mapping-test", 0); assert(backing[i] >= 0);
        assert(ftruncate(backing[i], (off_t)sizes[i]) == 0);
        broker_pages[i] = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, backing[i], 0);
        assert(broker_pages[i] != MAP_FAILED);
    }
    const struct lpr_unix_client client = { .fd = 100, .session = 1, .process_token = 101 };
    struct lpr_unix_mapping first, second;
    for (mode = EXTRA_WRITE; mode <= DUPLICATE_CAP; mode++) {
        initialize(UNIX_TRANSPORT_STREAM);
        assert(lpr_unix_mapping_attach(&client, 10, socket_type, 1, &first) < 0);
        assert(!active_maps && !first.socket);
        for (unsigned i = 16; i < 256; i++) assert(!caps[i].live);
        lpr_unix_mapping_destroy(&first);
    }
    mode = NORMAL;
    initialize(UNIX_TRANSPORT_STREAM);
    for (fail_map = 0; fail_map < 2; fail_map++) {
        map_calls = 0;
        assert(lpr_unix_mapping_attach(&client, 10, socket_type, 2, &first) == -ENOMEM);
        assert(!active_maps);
        for (unsigned i = 16; i < 256; i++) assert(!caps[i].live);
    }
    fail_map = -1;
    const uint32_t types[] = { UNIX_TRANSPORT_STREAM, UNIX_TRANSPORT_SEQPACKET };
    for (unsigned type = 0; type < 2; type++) {
        initialize(types[type]);
        peer_gone = 0;
        assert(lpr_unix_mapping_attach(&client, 10, socket_type, 3, &first) == 0);
        assert(lpr_unix_mapping_attach(&client, 20, socket_type, 4, &second) == 0);
        assert(active_maps == 4 && first.peer == second.socket && second.peer == first.socket);
        expect_readonly(first.outgoing_rx); expect_readonly(first.incoming_tx);
        expect_readonly(second.outgoing_rx); expect_readonly(second.incoming_tx);
        const unsigned before = rpc_calls;
        const char payload[] = "direct unix payload";
        struct unix_write write;
        assert(unix_transport_write_begin(first.outgoing_tx, first.outgoing_rx, first.generation,
            11, sizeof(payload) - 1, 0, 1, &write) == 0);
        memcpy(write.spans[0].base, payload, write.spans[0].length);
        assert(unix_transport_write_commit(first.outgoing_tx, &write) == 0);
        struct unix_read read;
        char copied[64] = {0};
        assert(unix_transport_read_begin(second.incoming_tx, second.incoming_rx, second.generation,
            22, sizeof(copied), &read) == 0);
        copy_read(&read, copied); assert(!strcmp(copied, payload));
        unix_transport_read_cancel(second.incoming_rx, &read); /* PEEK retains the record */
        assert(unix_transport_read_begin(second.incoming_tx, second.incoming_rx, second.generation,
            22, sizeof(copied), &read) == 0);
        assert(unix_transport_read_commit(second.incoming_rx, &read) == 0);
        if (socket_type == UNIX_TRANSPORT_SEQPACKET) {
            assert(unix_transport_write_begin(first.outgoing_tx, first.outgoing_rx, first.generation,
                11, 0, 0, 2, &write) == 0 && write.owner == 11);
            assert(unix_transport_write_commit(first.outgoing_tx, &write) == 0);
            assert(unix_transport_read_begin(second.incoming_tx, second.incoming_rx, second.generation,
                22, sizeof(copied), &read) == 0 && !read.length && !read.eof);
            assert(unix_transport_read_commit(second.incoming_rx, &read) == 0);
        }
        assert(unix_transport_write_begin(first.outgoing_tx, first.outgoing_rx, first.generation,
            11, 3, 0, 3, &write) == 0);
        memcpy(write.spans[0].base, "end", 3);
        assert(unix_transport_write_commit(first.outgoing_tx, &write) == 0);
        unix_transport_shutdown_tx(first.outgoing_tx);
        assert(rpc_calls == before); /* payload/PEEK/commit/shutdown needed no control call */
        lpr_unix_mapping_destroy(&second);
        peer_gone = 1;
        assert(lpr_unix_mapping_attach(&client, 20, socket_type, 5, &second) == 0 && !second.peer);
        assert(unix_transport_read_begin(second.incoming_tx, second.incoming_rx, second.generation,
            22, sizeof(copied), &read) == 0 && read.length == 3 && !read.eof);
        copy_read(&read, copied); assert(!memcmp(copied, "end", 3));
        assert(unix_transport_read_commit(second.incoming_rx, &read) == 0);
        assert(unix_transport_read_begin(second.incoming_tx, second.incoming_rx, second.generation,
            22, sizeof(copied), &read) == 0 && read.eof);
        assert(rpc_calls == before + 1); /* only the explicit reattach called unixd */
        lpr_unix_mapping_destroy(&first); lpr_unix_mapping_destroy(&first);
        lpr_unix_mapping_destroy(&second);
        assert(!active_maps);
    }
    sizes[0] = sizeof(struct unix_tx); sizes[1] = sizeof(struct unix_rx);
    struct unix_tx *dgram_tx = (void *)broker_pages[0];
    struct unix_rx *dgram_rx = (void *)broker_pages[1];
    assert(unix_transport_init(dgram_tx, dgram_rx, 17, UNIX_TRANSPORT_DGRAM) == 0);
    for (unsigned writing = 0; writing < 2; writing++) {
        for (unsigned failure = 0; failure < 4; failure++) {
            mode = failure == 1 ? WRONG_SIZE : NORMAL;
            fail_map = failure == 2 ? (int)map_calls + 1 : -1;
            struct pacha_ipc_fd received[2];
            for (unsigned i = 0; i < 2; i++) {
                caps[20 + i].live = 1; caps[20 + i].backing = i;
                caps[20 + i].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
                    PACHA_FD_RIGHT_MAP_READ |
                    ((i == 0) == !!writing ? PACHA_FD_RIGHT_MAP_WRITE : 0);
                if (failure == 3) caps[20 + i].rights |= PACHA_FD_RIGHT_MAP_WRITE;
                received[i] = (struct pacha_ipc_fd){ .fd = 20 + i };
            }
            struct lpr_unix_route_mapping route;
            int result = lpr_unix_route_import(received, 2, 17, writing, &route);
            assert((result == 0) == (failure == 0));
            if (!result) {
                expect_readonly(writing ? (void *)route.rx : (void *)route.tx);
                lpr_unix_route_destroy(&route);
            }
            assert(!active_maps && !caps[20].live && !caps[21].live);
        }
    }
    sizes[0] = sizes[1] = sizeof(struct unix_endpoint);
    for (unsigned i = 16; i < 256; i++) assert(!caps[i].live);
    for (unsigned i = 0; i < 2; i++) { assert(munmap(broker_pages[i], sizes[i]) == 0); close(backing[i]); }
    puts("lpr unix mapping: rights/size/generation validation, partial-map cleanup, real RO protection, STREAM/SEQPACKET and DGRAM routes passed");
}
