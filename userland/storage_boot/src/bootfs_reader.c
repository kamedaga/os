#include "bootfs_reader.h"
#include "../../kobox2_owner/bootfs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int storage_bootfs_read_file(pacha_fd_t fd, uint64_t archive_size, const char *path,
    unsigned char **out_data, uint64_t *out_size)
{
    if (!out_data || !out_size) return -EINVAL;
    *out_data = NULL;
    *out_size = 0;
    struct ph_bootfs archive = {0};
    const unsigned char *bytes = NULL;
    size_t size = 0;
    int error = ph_bootfs_open(&archive, fd, archive_size);
    if (!error) error = ph_bootfs_find(&archive, path, &bytes, &size);
    unsigned char *copy = NULL;
    if (!error) {
        copy = malloc(size ? size : 1);
        if (!copy) error = -ENOMEM;
        else memcpy(copy, bytes, size);
    }
    int closed = ph_bootfs_close(&archive);
    if (!error) error = closed;
    if (error) { free(copy); return error; }
    *out_data = copy;
    *out_size = size;
    return 0;
}
