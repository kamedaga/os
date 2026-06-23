#include <stdio.h>
#include <string.h>

#include "filed/vfs.h"

static int failures;

static void expect_true(const char *name, bool value)
{
    if (!value) {
        printf("FAIL %s\n", name);
        ++failures;
    }
}

static void expect_status(const char *name, filed_status_t got, filed_status_t expected)
{
    if (got != expected) {
        printf(
            "FAIL %s got=%s expected=%s\n",
            name,
            filed_status_name(got),
            filed_status_name(expected));
        ++failures;
    }
}

static void test_init_and_root_mount(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root = 0;
    filed_status_t status;

    filed_vfs_init(&vfs);
    status = filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root);

    expect_status("mount_root status", status, FILED_OK);
    expect_true("mount_root id", root == 1);
    expect_status("basic invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_mount_table_full(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root = 0;
    filed_status_t status = FILED_OK;
    unsigned int i;

    filed_vfs_init(&vfs);
    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        status = filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, i + 1, i + 1, &root);
        expect_status("mount table fill", status, FILED_OK);
    }

    status = filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 99, 99, &root);
    expect_status("mount table full", status, FILED_ERR_FULL);
    expect_status("full preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_open_close_and_prepare(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_io_decision_t decision;
    filed_status_t status;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for open",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    memset(&root, 0, sizeof(root));
    status = filed_vfs_open_root(
        &vfs,
        root_mount,
        FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
        FILED_OPEN_DIRECTORY,
        &root);
    expect_status("open root", status, FILED_OK);
    expect_true("root handle assigned", root.handle_id != 0);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "root lookup prepare",
        filed_vfs_lookup_prepare(&vfs, root.handle_id, &decision),
        FILED_OK);
    expect_true("root backend object", decision.backend_object == 11);

    memset(&file, 0, sizeof(file));
    status = filed_vfs_open_backend_child(
        &vfs,
        root.handle_id,
        42,
        FILED_VNODE_REGULAR,
        "hello.txt",
        FILED_RIGHT_READ | FILED_RIGHT_STAT,
        0,
        &file);
    expect_status("open regular child", status, FILED_OK);
    expect_true("file handle assigned", file.handle_id != 0 && file.handle_id != root.handle_id);

    memset(&decision, 0, sizeof(decision));
    status = filed_vfs_pread_prepare(&vfs, file.handle_id, 3, 9, &decision);
    expect_status("pread regular", status, FILED_OK);
    expect_true("pread backend object", decision.backend_object == 42);
    expect_true("pread offset length", decision.offset == 3 && decision.length == 9);

    memset(&decision, 0, sizeof(decision));
    status = filed_vfs_read_prepare(&vfs, file.handle_id, 7, &decision);
    expect_status("read regular", status, FILED_OK);
    expect_true("read starts at open file offset", decision.offset == 0 && decision.length == 7);
    expect_status("read commit", filed_vfs_read_commit(&vfs, file.handle_id, 4), FILED_OK);

    memset(&decision, 0, sizeof(decision));
    status = filed_vfs_pread_prepare(&vfs, file.handle_id, 3, 9, &decision);
    expect_status("pread after read", status, FILED_OK);
    expect_true("pread does not use shared offset", decision.offset == 3 && decision.length == 9);

    memset(&decision, 0, sizeof(decision));
    status = filed_vfs_read_prepare(&vfs, file.handle_id, 1, &decision);
    expect_status("read after commit", status, FILED_OK);
    expect_true("read offset advanced by bytes read", decision.offset == 4 && decision.length == 1);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "getdents regular rejects",
        filed_vfs_getdents_prepare(&vfs, file.handle_id, &decision),
        FILED_ERR_DENIED);

    expect_status("close file", filed_vfs_close_handle(&vfs, file.handle_id), FILED_OK);
    expect_status(
        "closed handle invalid",
        filed_vfs_pread_prepare(&vfs, file.handle_id, 0, 1, &decision),
        FILED_ERR_INVALID);
    expect_status("open close preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_rights_denial_and_kind_checks(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t readonly_file;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for denial",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for denial",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open child stat-only",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            43,
            FILED_VNODE_REGULAR,
            "stat-only",
            FILED_RIGHT_STAT,
            0,
            &readonly_file),
        FILED_OK);
    expect_status(
        "read right denied",
        filed_vfs_pread_prepare(&vfs, readonly_file.handle_id, 0, 1, &decision),
        FILED_ERR_DENIED);
    expect_status(
        "directory pread rejects",
        filed_vfs_pread_prepare(&vfs, root.handle_id, 0, 1, &decision),
        FILED_ERR_DENIED);
}

static void test_self_and_parent_open(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t dir;
    filed_vfs_open_result_t same_dir;
    filed_vfs_open_result_t parent;
    filed_vfs_open_result_t root_parent;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for parent",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for parent",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open directory child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            44,
            FILED_VNODE_DIRECTORY,
            "etc",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &dir),
        FILED_OK);
    expect_status(
        "open existing self",
        filed_vfs_open_existing(
            &vfs,
            dir.handle_id,
            FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &same_dir),
        FILED_OK);
    expect_status(
        "self stat prepare",
        filed_vfs_stat_prepare(&vfs, same_dir.handle_id, &decision),
        FILED_OK);
    expect_true("self backend object", decision.backend_object == 44);

    expect_status(
        "open parent",
        filed_vfs_open_parent(
            &vfs,
            dir.handle_id,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &parent),
        FILED_OK);
    expect_status(
        "parent lookup prepare",
        filed_vfs_lookup_prepare(&vfs, parent.handle_id, &decision),
        FILED_OK);
    expect_true("parent is root backend object", decision.backend_object == 11);

    expect_status(
        "root parent stays root",
        filed_vfs_open_parent(
            &vfs,
            root.handle_id,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root_parent),
        FILED_OK);
    expect_status(
        "root parent lookup prepare",
        filed_vfs_lookup_prepare(&vfs, root_parent.handle_id, &decision),
        FILED_OK);
    expect_true("root parent backend object", decision.backend_object == 11);
    expect_status("parent preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_directory_offset_and_exec_dup(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_open_result_t cloexec_file;
    filed_vfs_io_decision_t decision;
    filed_handle_id_t exec_handle = 0;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for offsets",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for offsets",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "getdents uses initial offset",
        filed_vfs_getdents_prepare(&vfs, root.handle_id, &decision),
        FILED_OK);
    expect_true("getdents initial offset", decision.offset == 0);
    expect_status("getdents commit", filed_vfs_getdents_commit(&vfs, root.handle_id, 2), FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "getdents uses committed offset",
        filed_vfs_getdents_prepare(&vfs, root.handle_id, &decision),
        FILED_OK);
    expect_true("getdents offset advanced", decision.offset == 2);

    expect_status(
        "open exec-inheritable child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            45,
            FILED_VNODE_REGULAR,
            "exec.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);
    expect_status("read before exec dup", filed_vfs_read_prepare(&vfs, file.handle_id, 8, &decision), FILED_OK);
    expect_status("advance source before exec dup", filed_vfs_read_commit(&vfs, file.handle_id, 5), FILED_OK);
    expect_status(
        "dup handle for exec",
        filed_vfs_dup_handle_for_exec(&vfs, file.handle_id, &exec_handle),
        FILED_OK);
    expect_true("exec dup handle assigned", exec_handle != 0 && exec_handle != file.handle_id);

    memset(&decision, 0, sizeof(decision));
    expect_status("exec dup shares offset", filed_vfs_read_prepare(&vfs, exec_handle, 2, &decision), FILED_OK);
    expect_true("exec dup sees source offset", decision.offset == 5);
    expect_status("exec dup commit shared", filed_vfs_read_commit(&vfs, exec_handle, 2), FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status("source sees exec dup offset", filed_vfs_read_prepare(&vfs, file.handle_id, 1, &decision), FILED_OK);
    expect_true("source offset advanced by dup", decision.offset == 7);

    expect_status(
        "open cloexec child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            46,
            FILED_VNODE_REGULAR,
            "secret.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            FILED_OPEN_CLOEXEC,
            &cloexec_file),
        FILED_OK);
    expect_status(
        "cloexec handle not inherited",
        filed_vfs_dup_handle_for_exec(&vfs, cloexec_file.handle_id, &exec_handle),
        FILED_ERR_DENIED);
    expect_status("directory offset preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_dup_and_flags(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_io_decision_t decision;
    filed_vfs_handle_flags_t flags;
    filed_handle_id_t dup_handle = 0;
    filed_handle_id_t exec_handle = 0;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for dup flags",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for dup flags",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open child append",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            47,
            FILED_VNODE_REGULAR,
            "flags.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            FILED_OPEN_APPEND,
            &file),
        FILED_OK);

    memset(&flags, 0, sizeof(flags));
    expect_status(
        "get initial flags",
        filed_vfs_get_handle_flags(&vfs, file.handle_id, &flags),
        FILED_OK);
    expect_true("initial fd flags clear", flags.fd_flags == 0);
    expect_true("initial status append", flags.status_flags == FILED_FILE_APPEND);

    expect_status(
        "dup with cloexec",
        filed_vfs_dup_handle(&vfs, file.handle_id, FILED_FD_CLOEXEC, &dup_handle),
        FILED_OK);
    expect_true("dup assigned", dup_handle != 0 && dup_handle != file.handle_id);

    memset(&flags, 0, sizeof(flags));
    expect_status(
        "dup flags visible",
        filed_vfs_get_handle_flags(&vfs, dup_handle, &flags),
        FILED_OK);
    expect_true("dup fd cloexec", flags.fd_flags == FILED_FD_CLOEXEC);
    expect_true("dup shares status append", flags.status_flags == FILED_FILE_APPEND);

    memset(&decision, 0, sizeof(decision));
    expect_status("dup read prepare", filed_vfs_read_prepare(&vfs, dup_handle, 9, &decision), FILED_OK);
    expect_true("dup read starts zero", decision.offset == 0);
    expect_status("dup read commit", filed_vfs_read_commit(&vfs, dup_handle, 6), FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status("source sees dup read offset", filed_vfs_read_prepare(&vfs, file.handle_id, 1, &decision), FILED_OK);
    expect_true("source offset after dup commit", decision.offset == 6);

    flags.fd_flags = 0;
    flags.status_flags = FILED_FILE_NONBLOCK | FILED_FILE_SYNC;
    expect_status("set dup flags", filed_vfs_set_handle_flags(&vfs, dup_handle, &flags), FILED_OK);

    memset(&flags, 0, sizeof(flags));
    expect_status(
        "source flags after dup set",
        filed_vfs_get_handle_flags(&vfs, file.handle_id, &flags),
        FILED_OK);
    expect_true("source fd flags independent", flags.fd_flags == 0);
    expect_true(
        "source sees shared status",
        flags.status_flags == (FILED_FILE_NONBLOCK | FILED_FILE_SYNC));

    flags.fd_flags = FILED_FD_CLOEXEC;
    flags.status_flags = FILED_FILE_NONBLOCK | FILED_FILE_SYNC;
    expect_status("restore dup cloexec", filed_vfs_set_handle_flags(&vfs, dup_handle, &flags), FILED_OK);
    expect_status(
        "dup cloexec blocks exec inherit",
        filed_vfs_dup_handle_for_exec(&vfs, dup_handle, &exec_handle),
        FILED_ERR_DENIED);
    expect_status(
        "source remains exec inheritable",
        filed_vfs_dup_handle_for_exec(&vfs, file.handle_id, &exec_handle),
        FILED_OK);

    expect_status("close exec dup", filed_vfs_close_handle(&vfs, exec_handle), FILED_OK);
    expect_status("close dup", filed_vfs_close_handle(&vfs, dup_handle), FILED_OK);
    expect_status("close source", filed_vfs_close_handle(&vfs, file.handle_id), FILED_OK);
    expect_status("dup flags preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_rights_and_flags(void)
{
    uint32_t rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_STAT;
    uint32_t open_flags =
        FILED_OPEN_CLOEXEC | FILED_OPEN_APPEND | FILED_OPEN_NONBLOCK | FILED_OPEN_SYNC;

    expect_true(
        "rights include subset",
        filed_rights_include(rights, FILED_RIGHT_LOOKUP | FILED_RIGHT_READ));
    expect_true("rights reject extra", !filed_rights_include(rights, FILED_RIGHT_WRITE));
    expect_true(
        "fd flags from open",
        filed_fd_flags_from_open(open_flags) == FILED_FD_CLOEXEC);
    expect_true(
        "file status flags from open",
        filed_file_status_flags_from_open(open_flags) ==
            (FILED_FILE_APPEND | FILED_FILE_NONBLOCK | FILED_FILE_SYNC));
}

static void test_invalid_arguments(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root = 0;

    filed_vfs_init(&vfs);
    expect_status("mount null vfs", filed_vfs_mount_root(NULL, FILED_FS_SYNTHETIC, 0, 0, &root), FILED_ERR_INVALID);
    expect_status("mount null out", filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 0, 0, NULL), FILED_ERR_INVALID);
    expect_status("check null vfs", filed_vfs_check_basic(NULL), FILED_ERR_INVALID);
}

int main(void)
{
    test_init_and_root_mount();
    test_mount_table_full();
    test_open_close_and_prepare();
    test_rights_denial_and_kind_checks();
    test_self_and_parent_open();
    test_directory_offset_and_exec_dup();
    test_dup_and_flags();
    test_rights_and_flags();
    test_invalid_arguments();

    if (failures != 0) {
        printf("filed vfs tests failed: %d\n", failures);
        return 1;
    }

    printf("filed vfs tests passed\n");
    return 0;
}
