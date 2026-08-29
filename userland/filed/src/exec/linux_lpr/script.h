#pragma once

#include <stddef.h>

#include "filed/payload.h"

enum {
    LPR_EXEC_SCRIPT_PREFIX_BYTES = 128,
    LPR_EXEC_SCRIPT_MAX_DEPTH = 4,
};

typedef int (*lpr_exec_script_open_prefix_fn)(
    void *context,
    const char *path,
    unsigned char *out_prefix,
    size_t prefix_capacity,
    size_t *out_prefix_length);

int lpr_exec_parse_shebang(
    const unsigned char *prefix,
    size_t prefix_length,
    char *out_interpreter,
    size_t interpreter_capacity,
    char *out_argument,
    size_t argument_capacity);

int lpr_exec_rewrite_script_request(
    filed_exec_path_t *request,
    const char *interpreter,
    const char *argument,
    const char *script_path);

int lpr_exec_resolve_script_chain(
    filed_exec_path_t *request,
    const unsigned char *initial_prefix,
    size_t initial_prefix_length,
    lpr_exec_script_open_prefix_fn open_prefix,
    void *context,
    char *out_executable_path,
    size_t executable_path_capacity);
