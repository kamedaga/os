#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../userland/koboxd/src/fs_backend.c"

static int in_kernel, cleanup_count, use_cleanup, link_error;
static char link_data[256];

unsigned long kb_module_kernel_gs_for_address(const void *address)
{
    assert(address != NULL);
    return 123;
}

int kb_shim_enter_kernel_gs(unsigned long gs, unsigned long *previous)
{
    assert(gs == 123 && !in_kernel);
    *previous = 456;
    in_kernel = 1;
    return 0;
}

void kb_shim_leave_kernel_gs(unsigned long previous)
{
    assert(previous == 456 && in_kernel);
    in_kernel = 0;
}

static void release_link(void *argument)
{
    assert(in_kernel && argument == link_data);
    ++cleanup_count;
    memset(link_data, '!', sizeof(link_data));
}

static const char *get_link(void *dentry, void *inode, void *delayed)
{
    assert(in_kernel && dentry && inode);
    if (use_cleanup) {
        void (*callback)(void *) = release_link;
        void *argument = link_data;
        memcpy(delayed, &callback, sizeof(callback));
        memcpy((char *)delayed + sizeof(callback), &argument, sizeof(argument));
    }
    if (link_error) return (const char *)(intptr_t)link_error;
    return link_data;
}

int main(void)
{
    static koboxd_fs_backend_t backend;
    uint8_t inode[128] = {0}, operations[128] = {0};
    void *ops_pointer = operations;
    const char *(*get_link_pointer)(void *, void *, void *) = get_link;
    memcpy(inode + KOBOXD_INODE_OP_OFFSET, &ops_pointer, sizeof(ops_pointer));
    memcpy(operations + KOBOXD_INODE_OP_GET_LINK_OFFSET, &get_link_pointer, sizeof(get_link_pointer));
    backend.mounted = 1;
    koboxd_fs_object_t *object = fs_objects_grow(&backend);
    assert(object);
    *object = (koboxd_fs_object_t) {
        .used = 1, .object_id = 1, .mode = 0120777, .inode = inode, .dentry = inode,
    };
    const size_t lengths[] = {1, 59, 60, 66, 255};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        for (size_t capacity = 3; capacity <= 256; capacity += 253) {
            memset(link_data, 'x', sizeof(link_data));
            link_data[lengths[i]] = '\0';
            object->size = lengths[i];
            use_cleanup = lengths[i] >= 60;
            cleanup_count = 0;
            char output[257];
            memset(output, '?', sizeof(output));
            size_t length = 999;
            assert(koboxd_fs_backend_readlink(&backend, 1, output, capacity, &length) == 0);
            assert(length == (lengths[i] < capacity ? lengths[i] : capacity));
            for (size_t j = 0; j < length; ++j) assert(output[j] == 'x');
            assert(output[length] == '?'); /* readlink must not append NUL. */
            assert(cleanup_count == use_cleanup && !in_kernel);
        }
    }
    char output[256];
    size_t length;
    use_cleanup = 1;
    cleanup_count = 0;
    link_error = -5;
    assert(koboxd_fs_backend_readlink(&backend, 1, output, sizeof(output), &length) == -5);
    assert(length == 0 && cleanup_count == 1 && !in_kernel);
    link_error = 0;
    memset(operations, 0, sizeof(operations));
    assert(koboxd_fs_backend_readlink(&backend, 1, output, sizeof(output), &length) == -95);
    assert(koboxd_fs_backend_readlink(&backend, 2, output, sizeof(output), &length) == -2);
    object->mode = 0100644;
    assert(koboxd_fs_backend_readlink(&backend, 1, output, sizeof(output), &length) == -22);
    memset(object, 0, sizeof(*object));
    fs_objects_trim_empty(&backend);
    puts("kobox ext4 readlink: OK (short/long/truncated/error/cleanup)");
}
