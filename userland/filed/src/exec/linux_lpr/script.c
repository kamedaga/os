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

static int append_string(char *strings, size_t capacity, uint64_t *used,
                          filed_exec_string_ref_t *ref, const char *value)
{
    if (!value) return -22;
    size_t size = strlen(value)+1;
    if (*used > capacity || size > capacity-*used) return -7;
    *ref = (filed_exec_string_ref_t){*used,size};
    memcpy(strings + *used, value, size);
    *used += size;
    return 0;
}
int lpr_exec_rewrite_script_request(filed_exec_path_t *r,
    const char *interpreter, const char *argument, const char *path)
{
    if (!r || !interpreter || !*interpreter || !argument || !path || !*path)
        return -22;
    uint64_t tail = r->argc ? r->argc-1 : 0;
    uint64_t extra = *argument ? 3 : 2;
    if (tail > UINT64_MAX-extra) return -7;
    uint64_t argc = tail+extra;
    if (argc > SIZE_MAX / sizeof(filed_exec_string_ref_t) ||
        r->envc > SIZE_MAX / sizeof(filed_exec_string_ref_t)-argc) return -7;
    size_t bytes = strlen(interpreter)+1;
    size_t addition = strlen(path)+1 + (*argument ? strlen(argument)+1 : 0);
    if (addition > SIZE_MAX-bytes) return -7;
    bytes += addition;
    for (unsigned list=0; list<2; ++list) {
        uint64_t start = list ? 0 : 1;
        uint64_t count = list ? r->envc : r->argc;
        filed_exec_string_ref_t *refs = list ? r->envp : r->argv;
        for (uint64_t i=start; i<count; ++i) {
            if (!filed_exec_string_ref_valid(r,refs[i])) return -22;
            if (refs[i].length > SIZE_MAX-bytes) return -7;
            bytes += (size_t)refs[i].length;
        }
    }
    size_t array_bytes = (size_t)(argc+r->envc)*sizeof(filed_exec_string_ref_t);
    if (bytes > SIZE_MAX-array_bytes) return -7;
    void *owned = malloc(array_bytes+bytes);
    if (!owned) return -12;
    filed_exec_string_ref_t *argv = owned, *envp = argv+argc;
    char *strings = (char *)owned+array_bytes;
    uint64_t used=0,index=0;
    int status = append_string(strings,bytes,&used,&argv[index++],interpreter);
    if (!status && *argument) status=append_string(strings,bytes,&used,&argv[index++],argument);
    if (!status) status=append_string(strings,bytes,&used,&argv[index++],path);
    for (uint64_t i=1; !status && i<r->argc; ++i)
        status=append_string(strings,bytes,&used,&argv[index++],filed_exec_string(r,r->argv[i]));
    for (uint64_t i=0; !status && i<r->envc; ++i)
        status=append_string(strings,bytes,&used,&envp[i],filed_exec_string(r,r->envp[i]));
    if (status) { free(owned); return status; }
    /* resolved_request borrows launch grants from the immutable request.
     * Its allocation owns only the rewritten argv/env strings. */
    free(r->allocation);
    r->allocation=owned;
    r->argv=argv;
    r->envp=envp;
    r->strings=strings;
    r->argc=argc;
    r->string_bytes=used;
    return 0;
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
        !request->path || strlen(request->path) >= 4096)
    {
        return -22;
    }

    unsigned char prefix[LPR_EXEC_SCRIPT_PREFIX_BYTES];
    char current_path[4096];
    char interpreter[4096];
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
