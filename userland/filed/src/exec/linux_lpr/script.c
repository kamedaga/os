#include "script.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int lpr_exec_script_space(unsigned char c)
{
    return c == ' ' || c == '\t';
}

int lpr_exec_parse_shebang(
    const unsigned char *prefix,
    size_t prefix_length,
    char *out_interpreter,
    size_t interpreter_capacity,
    char *out_argument,
    size_t argument_capacity)
{
    if (prefix == NULL || out_interpreter == NULL || interpreter_capacity == 0 ||
        out_argument == NULL || argument_capacity == 0 ||
        prefix_length > LPR_EXEC_SCRIPT_PREFIX_BYTES)
    {
        return -22;
    }
    out_interpreter[0] = '\0';
    out_argument[0] = '\0';
    if (prefix_length == LPR_EXEC_SCRIPT_PREFIX_BYTES) {
        prefix_length--;
    }
    if (prefix_length < 2 || prefix[0] != '#' || prefix[1] != '!') {
        return 0;
    }

    size_t line_end = 2;
    while (line_end < prefix_length && prefix[line_end] != '\n' && prefix[line_end] != '\0') {
        ++line_end;
    }
    while (line_end > 2 && lpr_exec_script_space(prefix[line_end - 1u])) {
        --line_end;
    }

    size_t interpreter_start = 2;
    while (interpreter_start < line_end && lpr_exec_script_space(prefix[interpreter_start])) {
        ++interpreter_start;
    }
    if (interpreter_start == line_end) {
        return -8;
    }
    size_t interpreter_end = interpreter_start;
    while (interpreter_end < line_end && !lpr_exec_script_space(prefix[interpreter_end])) {
        ++interpreter_end;
    }
    const size_t interpreter_length = interpreter_end - interpreter_start;
    if (interpreter_length + 1u > interpreter_capacity) {
        return -8;
    }
    memcpy(out_interpreter, prefix + interpreter_start, interpreter_length);
    out_interpreter[interpreter_length] = '\0';

    size_t argument_start = interpreter_end;
    while (argument_start < line_end && lpr_exec_script_space(prefix[argument_start])) {
        ++argument_start;
    }
    if (argument_start < line_end) {
        const size_t argument_length = line_end - argument_start;
        if (argument_length + 1u > argument_capacity) {
            out_interpreter[0] = '\0';
            return -8;
        }
        memcpy(out_argument, prefix + argument_start, argument_length);
        out_argument[argument_length] = '\0';
    }
    return 1;
}

static int lpr_exec_script_append(
    char *strings,
    uint64_t *string_bytes,
    filed_exec_string_ref_t *out_ref,
    const char *value)
{
    if (strings == NULL || string_bytes == NULL || out_ref == NULL || value == NULL) {
        return -22;
    }
    const size_t length = strlen(value) + 1u;
    if (length > UINT16_MAX || *string_bytes > FILED_EXEC_STRING_BYTES ||
        length > FILED_EXEC_STRING_BYTES - *string_bytes)
    {
        return -7;
    }
    out_ref->offset = (uint16_t)*string_bytes;
    out_ref->length = (uint16_t)length;
    memcpy(strings + *string_bytes, value, length);
    *string_bytes += length;
    return 0;
}

int lpr_exec_rewrite_script_request(
    filed_exec_path_t *request,
    const char *interpreter,
    const char *argument,
    const char *script_path)
{
    if (request == NULL || interpreter == NULL || interpreter[0] == '\0' ||
        argument == NULL || script_path == NULL || script_path[0] == '\0' ||
        request->argc > FILED_EXEC_MAX_ARGS || request->envc > FILED_EXEC_MAX_ENVS ||
        request->string_bytes > FILED_EXEC_STRING_BYTES)
    {
        return -22;
    }

    const uint64_t tail_count = request->argc > 0 ? request->argc - 1u : 0;
    const uint64_t argument_count = argument[0] != '\0' ? 1u : 0u;
    const uint64_t new_argc = 2u + argument_count + tail_count;
    if (new_argc > FILED_EXEC_MAX_ARGS) {
        return -7;
    }

    char *new_strings = malloc(FILED_EXEC_STRING_BYTES);
    if (new_strings == NULL) {
        return -12;
    }
    filed_exec_string_ref_t new_argv[FILED_EXEC_MAX_ARGS];
    filed_exec_string_ref_t new_envp[FILED_EXEC_MAX_ENVS];
    memset(new_argv, 0, sizeof(new_argv));
    memset(new_envp, 0, sizeof(new_envp));
    uint64_t new_string_bytes = 0;
    uint64_t new_index = 0;
    int status = lpr_exec_script_append(
        new_strings,
        &new_string_bytes,
        &new_argv[new_index++],
        interpreter);
    if (status == 0 && argument_count != 0) {
        status = lpr_exec_script_append(
            new_strings,
            &new_string_bytes,
            &new_argv[new_index++],
            argument);
    }
    if (status == 0) {
        status = lpr_exec_script_append(
            new_strings,
            &new_string_bytes,
            &new_argv[new_index++],
            script_path);
    }
    for (uint64_t i = 1; status == 0 && i < request->argc; ++i) {
        if (!filed_exec_string_ref_valid(request, request->argv[i])) {
            status = -22;
            break;
        }
        status = lpr_exec_script_append(
            new_strings,
            &new_string_bytes,
            &new_argv[new_index++],
            filed_exec_string(request, request->argv[i]));
    }
    for (uint64_t i = 0; status == 0 && i < request->envc; ++i) {
        if (!filed_exec_string_ref_valid(request, request->envp[i])) {
            status = -22;
            break;
        }
        status = lpr_exec_script_append(
            new_strings,
            &new_string_bytes,
            &new_envp[i],
            filed_exec_string(request, request->envp[i]));
    }
    if (status == 0) {
        memset(request->argv, 0, sizeof(request->argv));
        memset(request->envp, 0, sizeof(request->envp));
        memset(request->strings, 0, sizeof(request->strings));
        memcpy(request->argv, new_argv, sizeof(new_argv));
        memcpy(request->envp, new_envp, sizeof(new_envp));
        memcpy(request->strings, new_strings, (size_t)new_string_bytes);
        request->argc = new_argc;
        request->string_bytes = new_string_bytes;
    }
    free(new_strings);
    return status;
}

int lpr_exec_resolve_script_chain(
    filed_exec_path_t *request,
    const unsigned char *initial_prefix,
    size_t initial_prefix_length,
    lpr_exec_script_open_prefix_fn open_prefix,
    void *context,
    char *out_executable_path,
    size_t executable_path_capacity)
{
    if (request == NULL || initial_prefix == NULL ||
        initial_prefix_length > LPR_EXEC_SCRIPT_PREFIX_BYTES || open_prefix == NULL ||
        out_executable_path == NULL || executable_path_capacity == 0 ||
        strnlen(request->path, sizeof(request->path)) >= sizeof(request->path))
    {
        return -22;
    }

    unsigned char prefix[LPR_EXEC_SCRIPT_PREFIX_BYTES];
    char current_path[FILED_PATH_BYTES];
    char interpreter[FILED_PATH_BYTES];
    char argument[LPR_EXEC_SCRIPT_PREFIX_BYTES + 1u];
    memset(prefix, 0, sizeof(prefix));
    memcpy(prefix, initial_prefix, initial_prefix_length);
    size_t prefix_length = initial_prefix_length;
    memcpy(current_path, request->path, strlen(request->path) + 1u);

    for (unsigned int depth = 0;; ++depth) {
        const int parsed = lpr_exec_parse_shebang(
            prefix,
            prefix_length,
            interpreter,
            sizeof(interpreter),
            argument,
            sizeof(argument));
        if (parsed < 0) {
            return parsed;
        }
        if (parsed == 0) {
            const size_t path_length = strlen(current_path);
            if (path_length + 1u > executable_path_capacity) {
                return -36;
            }
            memcpy(out_executable_path, current_path, path_length + 1u);
            return 0;
        }
        if (depth >= LPR_EXEC_SCRIPT_MAX_DEPTH) {
            return -40;
        }
        int status = lpr_exec_rewrite_script_request(
            request,
            interpreter,
            argument,
            current_path);
        if (status != 0) {
            return status;
        }
        memset(prefix, 0, sizeof(prefix));
        prefix_length = 0;
        status = open_prefix(
            context,
            interpreter,
            prefix,
            sizeof(prefix),
            &prefix_length);
        if (status != 0) {
            return status;
        }
        if (prefix_length > sizeof(prefix)) {
            return -5;
        }
        memcpy(current_path, interpreter, strlen(interpreter) + 1u);
    }
}
