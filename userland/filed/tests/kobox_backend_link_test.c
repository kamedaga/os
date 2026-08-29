#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "filed/kobox_backend.h"
#include "pacha/trace.h"

static int failures;
static uint64_t wire_result;
static struct pacha_ipc_msg last_request;
static struct pacha_ipc_fd last_request_fd;
static union {
    max_align_t align;
    unsigned char bytes[STORAGE_PAGE_BYTES];
} wire_page;

typedef struct direct_link_state {
    int calls;
    uint64_t old_object_id;
    uint64_t new_parent_object_id;
    char new_name[STORAGE_NAME_BYTES];
} direct_link_state_t;

static void expect_int(const char *name, int got, int expected)
{
    if (got != expected) {
        fprintf(stderr, "%s: got %d expected %d\n", name, got, expected);
        ++failures;
    }
}

static void expect_u64(const char *name, uint64_t got, uint64_t expected)
{
    if (got != expected) {
        fprintf(stderr,
            "%s: got %llu expected %llu\n",
            name,
            (unsigned long long)got,
            (unsigned long long)expected);
        ++failures;
    }
}

static void expect_true(const char *name, int value)
{
    if (!value) {
        fprintf(stderr, "%s: expected true\n", name);
        ++failures;
    }
}

int filed_ipc_create_wire_page(uint64_t size, filed_page_t *out_page)
{
    if (out_page == NULL || size > sizeof(wire_page.bytes)) {
        return -22;
    }
    memset(&wire_page, 0, sizeof(wire_page));
    out_page->fd = 32;
    out_page->addr = wire_page.bytes;
    out_page->size = size;
    return 0;
}

void filed_ipc_destroy_wire_page(filed_page_t *page)
{
    if (page != NULL) {
        memset(page, 0, sizeof(*page));
        page->fd = -1;
    }
}

int filed_ipc_send_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd != 24 || msg == NULL) {
        return -22;
    }
    last_request = *msg;
    last_request.fds = NULL;
    if (msg->fd_count != 0 && msg->fds != NULL) {
        last_request_fd = msg->fds[0];
        last_request.fds = &last_request_fd;
    }
    return 0;
}

int filed_ipc_recv_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd != 24 || msg == NULL) {
        return -22;
    }
    msg->word0 = PACHA_SERVICE_REPLY_MAGIC;
    msg->word1 = 0;
    msg->word2 = wire_result;
    msg->word3 = last_request.word3;
    return 0;
}

uint64_t pacha_trace_name_id(const char *name)
{
    (void)name;
    return 0;
}

void pacha_trace_emit(
    uint32_t component,
    uint32_t event,
    uint32_t event_class,
    uint32_t arg_count,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5)
{
    (void)component;
    (void)event;
    (void)event_class;
    (void)arg_count;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
}

static int direct_link(
    void *ctx,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    direct_link_state_t *state = (direct_link_state_t *)ctx;
    ++state->calls;
    state->old_object_id = old_object_id;
    state->new_parent_object_id = new_parent_object_id;
    snprintf(state->new_name, sizeof(state->new_name), "%s", new_name);
    *out_object_id = 73;
    return 0;
}

static void test_direct_link(void)
{
    filed_kobox_backend_t backend;
    direct_link_state_t state;
    filed_kobox_direct_ops_t ops;
    uint64_t object_id = 0;

    memset(&state, 0, sizeof(state));
    memset(&ops, 0, sizeof(ops));
    ops.link = direct_link;
    filed_kobox_backend_init_direct(&backend, &state, &ops);

    expect_int(
        "direct link",
        filed_kobox_backend_link(&backend, 41, 8, "ld.bfd", &object_id),
        0);
    expect_int("direct link calls", state.calls, 1);
    expect_u64("direct link old object", state.old_object_id, 41);
    expect_u64("direct link new parent", state.new_parent_object_id, 8);
    expect_true("direct link name", strcmp(state.new_name, "ld.bfd") == 0);
    expect_u64("direct link result", object_id, 73);
    expect_u64("direct link dirty hint", filed_kobox_backend_dirty_hint(&backend), 1);
}

static void test_wire_link(void)
{
    filed_kobox_backend_t backend;
    uint64_t object_id = 0;

    memset(&last_request, 0, sizeof(last_request));
    memset(&last_request_fd, 0, sizeof(last_request_fd));
    wire_result = 88;
    filed_kobox_backend_init(&backend, 24);

    expect_int(
        "wire link",
        filed_kobox_backend_link(&backend, 52, 11, "toolchain/ar", &object_id),
        0);
    expect_u64("wire link op", last_request.word1, STORAGE_OP_LINK);
    expect_u64("wire link inline object", last_request.word2, 0);
    expect_u64("wire link fd count", last_request.fd_count, 1);
    expect_u64("wire link page fd", last_request_fd.fd, 32);

    const storage_link_request_t *link =
        (const storage_link_request_t *)wire_page.bytes;
    expect_u64("wire link old object", link->old_object_id, 52);
    expect_u64("wire link new parent", link->new_parent_object_id, 11);
    expect_true("wire link name", strcmp(link->new_name, "toolchain/ar") == 0);
    expect_u64("wire link result", object_id, 88);
    expect_u64("wire link dirty hint", filed_kobox_backend_dirty_hint(&backend), 1);

    filed_ipc_destroy_wire_page(&backend.wire_page);
}

int main(void)
{
    test_direct_link();
    test_wire_link();
    if (failures != 0) {
        return 1;
    }
    printf("filed kobox link backend tests passed\n");
    return 0;
}
