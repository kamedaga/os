/* SPDX-License-Identifier: MIT */
#include <kobox2/filesystem.h>
#include <assert.h>
#include <stdio.h>

static void expect(uint32_t opcode, uint64_t flags,
                   kb2_fs_operation_class_t class, uint64_t rights) {
    kb2_fs_request_t request = {.opcode = opcode, .flags = flags,
        .client = UINT64_MAX, .capabilities = UINT64_MAX,
        .uid = UINT32_MAX, .gid = UINT32_MAX};
    uint64_t required = UINT64_MAX;
    assert(kb2_fs_request_policy(&request, &required) == class);
    assert(required == rights);
}

int main(void) {
    const uint64_t expected[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        KB2_FILESYSTEM_RIGHT_LOOKUP | KB2_FILESYSTEM_RIGHT_READ, 0,
        KB2_FILESYSTEM_RIGHT_EXEC | KB2_FILESYSTEM_RIGHT_READ,
        KB2_FILESYSTEM_RIGHT_EXEC | KB2_FILESYSTEM_RIGHT_READ,
        KB2_FILESYSTEM_RIGHT_READ, KB2_FILESYSTEM_RIGHT_READ,
        KB2_FILESYSTEM_RIGHT_LOOKUP | KB2_FILESYSTEM_RIGHT_READ,
        0, 0, 0,
        KB2_FILESYSTEM_RIGHT_READ, KB2_FILESYSTEM_RIGHT_READ,
        KB2_FILESYSTEM_RIGHT_WRITE, KB2_FILESYSTEM_RIGHT_WRITE,
        0, KB2_FILESYSTEM_RIGHT_GETDENTS,
        KB2_FILESYSTEM_RIGHT_STAT, KB2_FILESYSTEM_RIGHT_STAT,
        0, 0, KB2_FILESYSTEM_RIGHT_WRITE,
        KB2_FILESYSTEM_RIGHT_CREATE, KB2_FILESYSTEM_RIGHT_REMOVE,
        KB2_FILESYSTEM_RIGHT_RENAME | KB2_FILESYSTEM_RIGHT_REMOVE,
        KB2_FILESYSTEM_RIGHT_CREATE, KB2_FILESYSTEM_RIGHT_CREATE,
        KB2_FILESYSTEM_RIGHT_STAT, KB2_FILESYSTEM_RIGHT_STAT,
        KB2_FILESYSTEM_RIGHT_CREATE,
        KB2_FILESYSTEM_RIGHT_SETATTR, KB2_FILESYSTEM_RIGHT_SETATTR,
        KB2_FILESYSTEM_RIGHT_SETATTR, KB2_FILESYSTEM_RIGHT_LOOKUP,
        0, 0, KB2_FILESYSTEM_RIGHT_CREATE,
    };
    assert(sizeof(expected) / sizeof(expected[0]) == KB2_FILESYSTEM_OPERATION_COUNT + 1);
    for (uint32_t op = 1; op <= KB2_FILESYSTEM_OPERATION_COUNT; ++op) {
        kb2_fs_operation_class_t class = KB2_FS_OPERATION_DATA;
        if ((op >= KB2_FILESYSTEM_OP_CLIENT_REGISTER &&
             op <= KB2_FILESYSTEM_OP_CLIENT_UNREGISTER) ||
            op == KB2_FILESYSTEM_OP_SERVICE_BIND || op == KB2_FILESYSTEM_OP_EXEC_OPEN)
            class = KB2_FS_OPERATION_OWNER;
        else if (op <= KB2_FILESYSTEM_OP_SHARED_FILE_VMO ||
                 op == KB2_FILESYSTEM_OP_TRANSFER_DUP)
            class = KB2_FS_OPERATION_FRONTEND;
        expect(op, 0, class, expected[op]);
    }
    uint64_t lookup = KB2_FILESYSTEM_RIGHT_LOOKUP;
    uint64_t read = KB2_FILESYSTEM_RIGHT_READ;
    uint64_t write = KB2_FILESYSTEM_RIGHT_WRITE;
    uint64_t create = KB2_FILESYSTEM_RIGHT_CREATE;
    expect(KB2_FILESYSTEM_OP_OPENAT2, 1, KB2_FS_OPERATION_DATA, lookup | write);
    expect(KB2_FILESYSTEM_OP_OPENAT2, 2, KB2_FS_OPERATION_DATA, lookup | read | write);
    expect(KB2_FILESYSTEM_OP_OPENAT2, 0100, KB2_FS_OPERATION_DATA, lookup | read | create);
    expect(KB2_FILESYSTEM_OP_OPENAT2, 01000, KB2_FS_OPERATION_DATA, lookup | read | write);
    expect(KB2_FILESYSTEM_OP_OPENAT2, 010000000 | 0100 | 01000 | 2,
           KB2_FS_OPERATION_DATA, lookup);
    expect(KB2_FILESYSTEM_OP_FILE_VMO, 2, KB2_FS_OPERATION_FRONTEND,
           read | KB2_FILESYSTEM_RIGHT_EXEC);
    expect(KB2_FILESYSTEM_OP_SHARED_FILE_VMO, 1, KB2_FS_OPERATION_FRONTEND, read | write);
    expect(KB2_FILESYSTEM_OP_SHARED_FILE_VMO, 2, KB2_FS_OPERATION_FRONTEND,
           read | KB2_FILESYSTEM_RIGHT_EXEC);
    expect(KB2_FILESYSTEM_OP_SHARED_FILE_VMO, 3, KB2_FS_OPERATION_FRONTEND,
           read | write | KB2_FILESYSTEM_RIGHT_EXEC);
    expect(0, 0, KB2_FS_OPERATION_INVALID, 0);
    expect(KB2_FILESYSTEM_OPERATION_COUNT + 1, 0, KB2_FS_OPERATION_INVALID, 0);
    expect(UINT32_MAX, 0, KB2_FS_OPERATION_INVALID, 0);
    uint64_t rights;
    kb2_fs_request_t snapshot={.opcode=KB2_FILESYSTEM_OP_PREAD,.mode=KB2_FILESYSTEM_SNAPSHOT_QUERY};
    assert(kb2_fs_request_policy(&snapshot,&rights)==KB2_FS_OPERATION_DATA);
    assert(rights==(KB2_FILESYSTEM_RIGHT_READ|KB2_FILESYSTEM_RIGHT_STAT));
    snapshot.mode=0;snapshot.mask=0x40000000u;
    assert(kb2_fs_request_policy(&snapshot,&rights)==KB2_FS_OPERATION_DATA);
    assert(rights==KB2_FILESYSTEM_RIGHT_READ);
    snapshot.opcode=KB2_FILESYSTEM_OP_STATX;
    assert(kb2_fs_request_policy(&snapshot,&rights)==KB2_FS_OPERATION_DATA);
    assert(rights==KB2_FILESYSTEM_RIGHT_STAT);
    assert(kb2_fs_request_policy(NULL, &rights) == KB2_FS_OPERATION_INVALID);
    kb2_fs_request_t request = {0};
    assert(kb2_fs_request_policy(&request, NULL) == KB2_FS_OPERATION_INVALID);
    puts("filesystem operation policy / claimed authority ignored: PASS");
}
