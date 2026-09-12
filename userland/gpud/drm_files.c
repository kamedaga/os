/* SPDX-License-Identifier: MIT */
#include "drm_files.h"

#include <errno.h>
#include <string.h>

static int generation_check(const struct gpud_drm_files *files, uint64_t generation) {
    if (!files || !files->generation)
        return -EINVAL;
    return files->generation == generation ? 0 : -ESTALE;
}

static int admit(const struct gpud_drm_files *files, uint64_t generation) {
    int error = generation_check(files, generation);
    return error ? error : files->terminal_error;
}

static struct gpud_drm_file *find_file(struct gpud_drm_files *files, uint64_t handle) {
    if (handle)
        for (size_t i = 0; i < files->limit; ++i)
            if (files->files[i].handle == handle)
                return &files->files[i];
    return NULL;
}

static int resolve(struct gpud_drm_files *files,
                   uint64_t generation,
                   uint64_t handle,
                   struct gpud_drm_file **out) {
    int error = admit(files, generation);
    if (error)
        return error;
    struct gpud_drm_file *file = find_file(files, handle);
    if (!file || !file->references)
        return -EBADF;
    if (file->state != GPUD_DRM_FILE_OPEN)
        return -EBUSY;
    *out = file;
    return 0;
}

int gpud_drm_files_init(struct gpud_drm_files *files, uint64_t generation, size_t limit) {
    if (!files || files->generation || !generation || !limit || limit > GPUD_DRM_FILES_MAX)
        return -EINVAL;
    *files = (struct gpud_drm_files){.generation = generation, .limit = limit};
    return 0;
}

int gpud_drm_file_open_begin(struct gpud_drm_files *files,
                             uint64_t generation,
                             uint64_t *handle_out) {
    int error = admit(files, generation);
    if (error)
        return error;
    if (!handle_out)
        return -EINVAL;
    if (files->handle_sequence == UINT64_MAX)
        return -EMFILE;
    for (size_t i = 0; i < files->limit; ++i) {
        struct gpud_drm_file *file = &files->files[i];
        if (file->state != GPUD_DRM_FILE_FREE)
            continue;
        *file = (struct gpud_drm_file){
            .handle = ++files->handle_sequence, .references = 1, .state = GPUD_DRM_FILE_OPENING};
        *handle_out = file->handle;
        return 0;
    }
    return -EMFILE;
}

int gpud_drm_file_open_finish(struct gpud_drm_files *files,
                              uint64_t generation,
                              uint64_t handle,
                              uint64_t session,
                              int error) {
    int checked = generation_check(files, generation);
    if (checked)
        return checked;
    struct gpud_drm_file *file = find_file(files, handle);
    if (!file || file->state != GPUD_DRM_FILE_OPENING || error > 0 ||
        (error ? session != 0 : !session))
        return -EINVAL;
    if (error) {
        memset(file, 0, sizeof(*file));
        return 0;
    }
    for (size_t i = 0; i < files->limit; ++i)
        if (&files->files[i] != file && files->files[i].session == session)
            return gpud_drm_files_fault(files, generation, -EPROTO);
    file->session = session;
    file->state = GPUD_DRM_FILE_OPEN;
    return 0;
}

int gpud_drm_file_dup(struct gpud_drm_files *files,
                      uint64_t generation,
                      uint64_t handle) {
    struct gpud_drm_file *file;
    int error = resolve(files, generation, handle, &file);
    if (error)
        return error;
    if (file->references == UINT32_MAX)
        return -EMFILE;
    ++file->references;
    return 0;
}

int gpud_drm_file_close(struct gpud_drm_files *files,
                        uint64_t generation,
                        uint64_t handle) {
    struct gpud_drm_file *file;
    int error = resolve(files, generation, handle, &file);
    if (error)
        return error;
    if (!--file->references)
        file->state = GPUD_DRM_FILE_DRAINING;
    return 0;
}

int gpud_drm_file_acquire(struct gpud_drm_files *files,
                          uint64_t generation,
                          uint64_t handle,
                          struct gpud_drm_binding *binding_out) {
    if (!binding_out)
        return -EINVAL;
    struct gpud_drm_file *file;
    int error = resolve(files, generation, handle, &file);
    if (error)
        return error;
    if (file->in_flight == UINT32_MAX)
        return -EBUSY;
    ++file->in_flight;
    *binding_out = (struct gpud_drm_binding){
        .generation = generation, .frontend_handle = handle, .session_id = file->session};
    return 0;
}

int gpud_drm_file_release(struct gpud_drm_files *files, uint64_t generation, uint64_t handle) {
    int error = generation_check(files, generation);
    if (error)
        return error;
    struct gpud_drm_file *file = find_file(files, handle);
    if (!file || !file->in_flight)
        return -EINVAL;
    --file->in_flight;
    return 0;
}

int gpud_drm_file_next_close(struct gpud_drm_files *files,
                             uint64_t generation,
                             struct gpud_drm_binding *binding_out) {
    int error = admit(files, generation);
    if (error)
        return error;
    if (!binding_out)
        return -EINVAL;
    for (size_t i = 0; i < files->limit; ++i) {
        struct gpud_drm_file *file = &files->files[i];
        if (file->state != GPUD_DRM_FILE_DRAINING || file->in_flight)
            continue;
        file->state = GPUD_DRM_FILE_CLOSING;
        *binding_out = (struct gpud_drm_binding){
            .generation = generation, .frontend_handle = file->handle, .session_id = file->session};
        return 1;
    }
    return 0;
}

int gpud_drm_file_close_finish(struct gpud_drm_files *files,
                               uint64_t generation,
                               uint64_t handle,
                               int error) {
    int checked = generation_check(files, generation);
    if (checked)
        return checked;
    struct gpud_drm_file *file = find_file(files, handle);
    if (!file || file->state != GPUD_DRM_FILE_CLOSING || error > 0)
        return -EINVAL;
    if (error) {
        file->state = GPUD_DRM_FILE_FAILED;
        return gpud_drm_files_fault(files, generation, error);
    }
    memset(file, 0, sizeof(*file));
    return 0;
}

int gpud_drm_files_fault(struct gpud_drm_files *files, uint64_t generation, int error) {
    int checked = generation_check(files, generation);
    if (checked)
        return checked;
    if (error >= 0)
        return -EINVAL;
    if (!files->terminal_error)
        files->terminal_error = error;
    return files->terminal_error;
}
