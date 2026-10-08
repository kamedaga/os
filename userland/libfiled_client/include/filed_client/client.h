/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <kobox2/filesystem.h>

/* One immutable request VMO per control call. Queue sessions use the same raw
 * codec; this helper is also used before a queue can be negotiated. */
int filed_client_call(int endpoint, const kb2_fs_request_t *request,
                      const void *tail, size_t tail_size,
                      void *output, size_t output_capacity,
                      kb2_fs_response_t *response,
                      const int *source_fds, size_t source_count,
                      int *reply_fds, size_t *reply_count);
int filed_client_call_published(int endpoint, const kb2_fs_request_t *request,
                      const void *tail, size_t tail_size,
                      void *output, size_t output_capacity,
                      kb2_fs_response_t *response,
                      const int *source_fds, size_t source_count,
                      int *reply_fds, size_t *reply_count, int *published);
int filedfs_call(int endpoint, kb2_fs_request_t *request,
                 const char *path, const char *second_path,
                 const void *data, size_t data_size,
                 void *output, size_t output_capacity,
                 kb2_fs_response_t *response,
                 const int *source_fds, size_t source_count,
                 int *reply_fds, size_t *reply_count);
