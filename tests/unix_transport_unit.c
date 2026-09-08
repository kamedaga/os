#define _GNU_SOURCE
#include "unixd/transport.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static struct unix_tx *tx;
static struct unix_rx *rx;
static const uint64_t generation = 17;

static void reset(uint32_t type)
{
    memset(tx, 0, sizeof(*tx));
    memset(rx, 0, sizeof(*rx));
    assert(unix_transport_init(tx, rx, generation, type) == 0);
}

static void write_copy(struct unix_write *write, const void *data)
{
    memcpy(write->spans[0].base, data, write->spans[0].length);
    memcpy(write->spans[1].base, (const char *)data + write->spans[0].length,
        write->spans[1].length);
}

static void read_copy(const struct unix_read *read, void *data)
{
    memcpy(data, read->spans[0].base, read->spans[0].length);
    memcpy((char *)data + read->spans[0].length, read->spans[1].base,
        read->spans[1].length);
}

static void send_bytes(const void *data, size_t length, uint64_t ticket)
{
    struct unix_write write;
    assert(unix_transport_write_begin(tx, rx, generation, 1, length,
        ticket, 73, &write) == 0);
    assert(write.length == length);
    write_copy(&write, data);
    assert(unix_transport_write_commit(tx, &write) == 0);
}

static void stream_and_wrap(void)
{
    reset(UNIX_TRANSPORT_STREAM);
    /* Cross the physical end AND uint32_t byte-position wrap. */
    tx->published = rx->consumed = UINT32_MAX - 31u;
    send_bytes("abcdef", 6, 91);
    struct unix_read read;
    char bytes[8] = {0};
    assert(unix_transport_read_begin(tx, rx, generation, 2, 2, &read) == 0);
    assert(read.length == 2 && read.ticket == 91);
    read_copy(&read, bytes);
    assert(memcmp(bytes, "ab", 2) == 0);
    unix_transport_read_cancel(rx, &read); /* PEEK */
    assert(unix_transport_read_begin(tx, rx, generation, 2, 2, &read) == 0);
    assert(read.ticket == 91);
    assert(unix_transport_read_commit(rx, &read) == 0);
    assert(unix_transport_read_begin(tx, rx, generation, 3, sizeof(bytes), &read) == 0);
    assert(read.ticket == 91 && read.length == 4);
    read_copy(&read, bytes);
    assert(memcmp(bytes, "cdef", 4) == 0);
    assert(unix_transport_read_commit(rx, &read) == 0);
    assert(unix_transport_read_begin(tx, rx, generation, 2, 8, &read) == -EAGAIN);
    unix_transport_shutdown_tx(tx);
    assert(unix_transport_read_begin(tx, rx, generation, 2, 8, &read) == 0);
    assert(read.eof && read.owner == 0);
}

static void packet_rules(void)
{
    reset(UNIX_TRANSPORT_SEQPACKET);
    send_bytes("", 0, 91);
    int ready, eof;
    assert(unix_transport_readable(tx, rx, generation, &ready, &eof) == 0);
    assert(ready && !eof);
    struct unix_read read;
    assert(unix_transport_read_begin(tx, rx, generation, 2, 0, &read) == 0);
    assert(!read.eof && read.owner == 2 && read.ticket == 91 && !read.truncated);
    assert(unix_transport_read_commit(rx, &read) == 0);
    send_bytes("abcdef", 6, 0);
    assert(unix_transport_read_begin(tx, rx, generation, 2, 2, &read) == 0);
    assert(read.truncated && read.message_length == 6 && read.length == 2);
    assert(unix_transport_read_commit(rx, &read) == 0);
    assert(unix_transport_read_begin(tx, rx, generation, 2, 8, &read) == -EAGAIN);
    struct unix_write write;
    assert(unix_transport_write_begin(tx, rx, generation, 1, UNIX_TRANSPORT_BYTES,
        0, 0, &write) == -EMSGSIZE);
    assert(tx->owner == 0);
}

static void capacity_and_shutdown(void)
{
    reset(UNIX_TRANSPORT_STREAM);
    struct unix_write write, other;
    assert(unix_transport_write_begin(tx, rx, generation, 1, SIZE_MAX, 0, 0, &write) == 0);
    assert(write.length == UNIX_TRANSPORT_BYTES - sizeof(struct unix_record));
    assert(unix_transport_write_begin(tx, rx, generation, 2, 1, 0, 0, &other) == -EBUSY);
    assert(unix_transport_write_commit(tx, &write) == 0);
    assert(unix_transport_write_begin(tx, rx, generation, 1, 1, 0, 0, &write) == -EAGAIN);
    int ready;
    assert(unix_transport_writable(tx, rx, generation, &ready) == 0 && !ready);
    unix_transport_shutdown_tx(tx);
    struct unix_read read;
    assert(unix_transport_read_begin(tx, rx, generation, 2, UNIX_TRANSPORT_BYTES, &read) == 0);
    assert(read.length == UNIX_TRANSPORT_BYTES - sizeof(struct unix_record) && !read.eof);
    assert(unix_transport_read_commit(rx, &read) == 0);
    assert(unix_transport_read_begin(tx, rx, generation, 2, 1, &read) == 0 && read.eof);

    reset(UNIX_TRANSPORT_STREAM);
    assert(unix_transport_write_begin(tx, rx, generation, 1, 3, 0, 0, &write) == 0);
    unix_transport_shutdown_tx(tx);
    assert(unix_transport_write_commit(tx, &write) == -EPIPE);
    assert((uint32_t)tx->published == 0);
    reset(UNIX_TRANSPORT_STREAM);
    unix_transport_shutdown_rx(rx);
    assert(unix_transport_write_begin(tx, rx, generation, 1, 3, 0, 0, &write) == -EPIPE);
}

static void wait_child(pid_t child, int signal_number)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (signal_number != 0) assert(WIFSIGNALED(status) && WTERMSIG(status) == signal_number);
    else assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void death_at_commit(void)
{
    for (unsigned committed = 0; committed < 2; committed++) {
        reset(UNIX_TRANSPORT_STREAM);
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct unix_write write;
            assert(unix_transport_write_begin(tx, rx, generation, 71, 4, 0, 0, &write) == 0);
            write_copy(&write, "live");
            if (committed) __atomic_store_n(&tx->published, write.after, __ATOMIC_RELEASE);
            _exit(0); /* die either side of commit, before unlock/notify */
        }
        wait_child(child, 0);
        assert(unix_transport_recover_tx(tx, 99) == 0);
        assert(unix_transport_recover_tx(tx, 71) == 1);
        assert(unix_transport_recover_tx(tx, 71) == 0);
        struct unix_read read;
        const int result = unix_transport_read_begin(tx, rx, generation, 2, 4, &read);
        assert(result == (committed ? 0 : -EAGAIN));
        if (committed) unix_transport_read_cancel(rx, &read);
    }
    for (unsigned committed = 0; committed < 2; committed++) {
        reset(UNIX_TRANSPORT_STREAM);
        send_bytes("live", 4, 0);
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct unix_read read;
            assert(unix_transport_read_begin(tx, rx, generation, 72, 2, &read) == 0);
            if (committed) __atomic_store_n(&rx->consumed, read.after, __ATOMIC_RELEASE);
            _exit(0);
        }
        wait_child(child, 0);
        assert(unix_transport_recover_rx(rx, 72) == 1);
        struct unix_read read;
        assert(unix_transport_read_begin(tx, rx, generation, 2, 4, &read) == 0);
        assert(read.length == (committed ? 2u : 4u));
        assert(unix_transport_read_commit(rx, &read) == 0);
    }
}

static void invalid_peer(void)
{
    reset(UNIX_TRANSPORT_STREAM);
    struct unix_write write;
    assert(unix_transport_write_begin(tx, rx, generation + 1, 1, 4, 0, 0, &write) == -ESTALE);
    rx->consumed = 32;
    assert(unix_transport_write_begin(tx, rx, generation, 1, 4, 0, 0, &write) == -EPROTO);
    rx->consumed = 0;
    send_bytes("abcd", 4, 0);
    struct unix_record *record = (void *)tx->data;
    record->length = UINT32_MAX;
    struct unix_read read;
    assert(unix_transport_read_begin(tx, rx, generation, 2, 4, &read) == -EPROTO);
    assert(rx->owner == 0 && rx->consumed == 0);
}

static void process_transfer(void)
{
    reset(UNIX_TRANSPORT_SEQPACKET);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(mprotect(rx, sizeof(*rx), PROT_READ) == 0);
        for (uint32_t sequence = 0; sequence < 10000; sequence++) {
            struct unix_write write;
            int status;
            do {
                status = unix_transport_write_begin(tx, rx, generation, 11,
                    sizeof(sequence), 0, sequence, &write);
            } while (status == -EAGAIN);
            assert(status == 0);
            write_copy(&write, &sequence);
            assert(unix_transport_write_commit(tx, &write) == 0);
        }
        unix_transport_shutdown_tx(tx);
        _exit(0);
    }
    assert(mprotect(tx, sizeof(*tx), PROT_READ) == 0);
    for (uint32_t sequence = 0; sequence < 10000; sequence++) {
        struct unix_read read;
        int status;
        do {
            status = unix_transport_read_begin(tx, rx, generation, 12,
                sizeof(sequence), &read);
        } while (status == -EAGAIN);
        assert(status == 0 && !read.eof && read.length == sizeof(sequence));
        uint32_t received;
        read_copy(&read, &received);
        assert(received == sequence && read.operation == sequence);
        assert(unix_transport_read_commit(rx, &read) == 0);
    }
    wait_child(child, 0);
    assert(mprotect(tx, sizeof(*tx), PROT_READ | PROT_WRITE) == 0);
}

static void readonly_peer_mapping(void)
{
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        signal(SIGSEGV, SIG_DFL);
        assert(mprotect(tx, sizeof(*tx), PROT_READ) == 0);
        *(volatile uint64_t *)&tx->published = 3;
        _exit(1);
    }
    wait_child(child, SIGSEGV);
}

int main(void)
{
    tx = mmap(NULL, sizeof(*tx), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    rx = mmap(NULL, sizeof(*rx), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(tx != MAP_FAILED && rx != MAP_FAILED);
    stream_and_wrap();
    packet_rules();
    capacity_and_shutdown();
    death_at_commit();
    invalid_peer();
    process_transfer();
    readonly_peer_mapping();
    assert(munmap(tx, sizeof(*tx)) == 0);
    assert(munmap(rx, sizeof(*rx)) == 0);
    puts("unix transport: stream/packet, wrap, pressure, PEEK, death, permissions, 10000 messages passed");
    return 0;
}
