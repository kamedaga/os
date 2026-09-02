#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exec/linux_lpr/script.h"

typedef struct fake_file {
    const char *path;
    const char *prefix;
    int status;
} fake_file_t;

typedef struct fake_files {
    const fake_file_t *files;
    size_t count;
    size_t opens;
} fake_files_t;

static void fail(const char *label)
{
    fprintf(stderr, "shebang_test: FAIL: %s\n", label);
    exit(1);
}

static void expect_status(const char *label, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "shebang_test: FAIL: %s actual=%d expected=%d\n", label, actual, expected);
        exit(1);
    }
}

static void expect_string(const char *label, const char *actual, const char *expected)
{
    if (actual == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr,
            "shebang_test: FAIL: %s actual=%s expected=%s\n",
            label,
            actual == NULL ? "(null)" : actual,
            expected);
        exit(1);
    }
}

static void add_string(
    filed_exec_path_t *request,
    filed_exec_string_ref_t *ref,
    const char *value)
{
    const size_t length = strlen(value) + 1u;
    if (request->string_bytes + length > sizeof(request->strings)) {
        fail("test request string capacity");
    }
    ref->offset = (uint16_t)request->string_bytes;
    ref->length = (uint16_t)length;
    memcpy(request->strings + request->string_bytes, value, length);
    request->string_bytes += length;
}

static void init_request(filed_exec_path_t *request, const char *path)
{
    memset(request, 0, sizeof(*request));
    if (snprintf(request->path, sizeof(request->path), "%s", path) >= (int)sizeof(request->path)) {
        fail("test request path capacity");
    }
    add_string(request, &request->argv[request->argc++], "discard-me");
    add_string(request, &request->argv[request->argc++], "tail-one");
    add_string(request, &request->argv[request->argc++], "tail-two");
    add_string(request, &request->envp[request->envc++], "SHEBANG_ENV=kept");
}

static const char *request_arg(const filed_exec_path_t *request, size_t index)
{
    if (index >= request->argc || !filed_exec_string_ref_valid(request, request->argv[index])) {
        return NULL;
    }
    return filed_exec_string(request, request->argv[index]);
}

static int fake_open_prefix(
    void *context_raw,
    const char *path,
    unsigned char *out_prefix,
    size_t prefix_capacity,
    size_t *out_prefix_length)
{
    fake_files_t *context = context_raw;
    context->opens++;
    for (size_t i = 0; i < context->count; ++i) {
        const fake_file_t *file = &context->files[i];
        if (strcmp(file->path, path) != 0) {
            continue;
        }
        if (file->status != 0) {
            return file->status;
        }
        const size_t length = strlen(file->prefix);
        if (length > prefix_capacity) {
            return -5;
        }
        memcpy(out_prefix, file->prefix, length);
        *out_prefix_length = length;
        return 0;
    }
    return -2;
}

static void test_argument_and_argv_order(void)
{
    static const fake_file_t files[] = {
        { "/bin/probe", "\177ELF", 0 },
    };
    fake_files_t context = { files, sizeof(files) / sizeof(files[0]), 0 };
    filed_exec_path_t request;
    init_request(&request, "/tmp/top-script");
    const unsigned char prefix[] = "#!  /bin/probe\t--flag with spaces  \nignored";
    char executable[FILED_PATH_BYTES];
    expect_status(
        "resolve argument script",
        lpr_exec_resolve_script_chain(
            &request,
            prefix,
            sizeof(prefix) - 1u,
            fake_open_prefix,
            &context,
            executable,
            sizeof(executable)),
        0);
    expect_string("resolved executable", executable, "/bin/probe");
    if (request.argc != 5) fail("rewritten argc");
    expect_string("argv 0", request_arg(&request, 0), "/bin/probe");
    expect_string("argv optional", request_arg(&request, 1), "--flag with spaces");
    expect_string("argv script", request_arg(&request, 2), "/tmp/top-script");
    expect_string("argv tail one", request_arg(&request, 3), "tail-one");
    expect_string("argv tail two", request_arg(&request, 4), "tail-two");
    if (request.envc != 1 || !filed_exec_string_ref_valid(&request, request.envp[0])) {
        fail("environment reference");
    }
    expect_string(
        "environment preserved",
        filed_exec_string(&request, request.envp[0]),
        "SHEBANG_ENV=kept");
}

static void test_four_level_nesting(void)
{
    static const fake_file_t files[] = {
        { "/s2", "#!/s3 a2\n", 0 },
        { "/s3", "#!/s4 a3\n", 0 },
        { "/s4", "#!/bin/final a4\n", 0 },
        { "/bin/final", "\177ELF", 0 },
    };
    fake_files_t context = { files, sizeof(files) / sizeof(files[0]), 0 };
    filed_exec_path_t request;
    init_request(&request, "/s1");
    const unsigned char prefix[] = "#!/s2 a1\n";
    char executable[FILED_PATH_BYTES];
    expect_status(
        "four nested scripts",
        lpr_exec_resolve_script_chain(
            &request,
            prefix,
            sizeof(prefix) - 1u,
            fake_open_prefix,
            &context,
            executable,
            sizeof(executable)),
        0);
    if (context.opens != 4) fail("four nested open count");
    expect_string("nested executable", executable, "/bin/final");
    static const char *expected[] = {
        "/bin/final", "a4", "/s4", "a3", "/s3", "a2", "/s2", "a1", "/s1",
        "tail-one", "tail-two",
    };
    if (request.argc != sizeof(expected) / sizeof(expected[0])) fail("nested argc");
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        expect_string("nested argv", request_arg(&request, i), expected[i]);
    }
}

static void test_nesting_limit(void)
{
    static const fake_file_t files[] = {
        { "/s1", "#!/s2\n", 0 },
        { "/s2", "#!/s3\n", 0 },
        { "/s3", "#!/s4\n", 0 },
        { "/s4", "#!/bin/final\n", 0 },
    };
    fake_files_t context = { files, sizeof(files) / sizeof(files[0]), 0 };
    filed_exec_path_t request;
    init_request(&request, "/s0");
    const unsigned char prefix[] = "#!/s1\n";
    char executable[FILED_PATH_BYTES];
    expect_status(
        "fifth nested script returns ELOOP",
        lpr_exec_resolve_script_chain(
            &request,
            prefix,
            sizeof(prefix) - 1u,
            fake_open_prefix,
            &context,
            executable,
            sizeof(executable)),
        -40);
    if (context.opens != 4) fail("nesting limit open count");
}

static void test_interpreter_open_errno(void)
{
    filed_exec_path_t request;
    char executable[FILED_PATH_BYTES];
    const unsigned char missing_prefix[] = "#!/missing\n";
    fake_files_t missing = { NULL, 0, 0 };
    init_request(&request, "/missing-script");
    expect_status(
        "missing interpreter returns ENOENT",
        lpr_exec_resolve_script_chain(
            &request,
            missing_prefix,
            sizeof(missing_prefix) - 1u,
            fake_open_prefix,
            &missing,
            executable,
            sizeof(executable)),
        -2);

    static const fake_file_t denied_files[] = {
        { "/not-executable", "", -13 },
    };
    fake_files_t denied = { denied_files, sizeof(denied_files) / sizeof(denied_files[0]), 0 };
    const unsigned char denied_prefix[] = "#!/not-executable\n";
    init_request(&request, "/denied-script");
    expect_status(
        "non-executable interpreter returns EACCES",
        lpr_exec_resolve_script_chain(
            &request,
            denied_prefix,
            sizeof(denied_prefix) - 1u,
            fake_open_prefix,
            &denied,
            executable,
            sizeof(executable)),
        -13);
}

static void test_127_byte_line_cutoff(void)
{
    unsigned char prefix[LPR_EXEC_SCRIPT_PREFIX_BYTES];
    memset(prefix, 'x', sizeof(prefix));
    prefix[0] = '#';
    prefix[1] = '!';
    prefix[126] = ' ';
    prefix[127] = 'Z';
    char interpreter[FILED_PATH_BYTES];
    char argument[LPR_EXEC_SCRIPT_PREFIX_BYTES + 1u];
    expect_status(
        "127 byte shebang cutoff",
        lpr_exec_parse_shebang(
            prefix,
            sizeof(prefix),
            interpreter,
            sizeof(interpreter),
            argument,
            sizeof(argument)),
        1);
    if (strlen(interpreter) != 124) fail("127 byte interpreter length");
    expect_string("byte 128 ignored", argument, "");
}

int main(void)
{
    test_argument_and_argv_order();
    test_four_level_nesting();
    test_nesting_limit();
    test_interpreter_open_errno();
    test_127_byte_line_cutoff();
    puts("shebang_test: PASS");
    return 0;
}
