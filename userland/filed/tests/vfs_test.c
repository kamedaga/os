#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "filed/vfs.h"

static int failures;

typedef struct concurrent_offset_args {
    filed_vfs_t *vfs;
    filed_handle_id_t handle_id;
    unsigned int iterations;
    filed_status_t status;
} concurrent_offset_args_t;

typedef struct concurrent_unlink_args {
    filed_vfs_t *vfs;
    filed_handle_id_t parent_handle_id;
    const char *name;
    filed_status_t status;
} concurrent_unlink_args_t;

typedef struct concurrent_rename_args {
    filed_vfs_t *vfs;
    filed_handle_id_t old_parent_handle_id;
    filed_handle_id_t new_parent_handle_id;
    const char *old_name;
    const char *new_name;
    filed_backend_object_id_t backend_object;
    filed_status_t status;
} concurrent_rename_args_t;

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

static void *concurrent_offset_worker(void *opaque)
{
    concurrent_offset_args_t *args = (concurrent_offset_args_t *)opaque;
    unsigned int i;

    args->status = FILED_OK;
    for (i = 0; i < args->iterations; ++i) {
        filed_status_t status = filed_vfs_read_commit(args->vfs, args->handle_id, 1);
        if (status != FILED_OK) {
            args->status = status;
            break;
        }
    }
    return NULL;
}

static void *concurrent_unlink_worker(void *opaque)
{
    concurrent_unlink_args_t *args = (concurrent_unlink_args_t *)opaque;
    args->status = filed_vfs_unlink_commit(
        args->vfs,
        args->parent_handle_id,
        args->name);
    return NULL;
}

static void *concurrent_rename_worker(void *opaque)
{
    concurrent_rename_args_t *args = (concurrent_rename_args_t *)opaque;
    args->status = filed_vfs_rename_commit(
        args->vfs,
        args->old_parent_handle_id,
        args->new_parent_handle_id,
        args->old_name,
        args->new_name,
        args->backend_object);
    return NULL;
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

static void test_write_pwrite_and_fsync(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_open_result_t append_file;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for write",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for write",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open writable child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            48,
            FILED_VNODE_REGULAR,
            "write.txt",
            FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "pwrite prepare",
        filed_vfs_pwrite_prepare(&vfs, file.handle_id, 9, 5, &decision),
        FILED_OK);
    expect_true("pwrite decision", decision.backend_object == 48 && decision.offset == 9 && decision.length == 5);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "write initial offset",
        filed_vfs_write_prepare(&vfs, file.handle_id, 6, &decision),
        FILED_OK);
    expect_true("write starts at zero", decision.offset == 0 && decision.length == 6);
    expect_status("write commit", filed_vfs_write_commit(&vfs, file.handle_id, 6), FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "pwrite after write",
        filed_vfs_pwrite_prepare(&vfs, file.handle_id, 2, 3, &decision),
        FILED_OK);
    expect_true("pwrite ignores shared offset", decision.offset == 2 && decision.length == 3);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "write after commit",
        filed_vfs_write_prepare(&vfs, file.handle_id, 1, &decision),
        FILED_OK);
    expect_true("write offset advanced", decision.offset == 6 && decision.length == 1);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "fsync prepare",
        filed_vfs_fsync_prepare(&vfs, file.handle_id, &decision),
        FILED_OK);
    expect_true("fsync backend object", decision.backend_object == 48);

    expect_status(
        "read-only fsync denied",
        filed_vfs_fsync_prepare(&vfs, root.handle_id, &decision),
        FILED_ERR_DENIED);

    expect_status(
        "open append child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            49,
            FILED_VNODE_REGULAR,
            "append.txt",
            FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
            FILED_OPEN_APPEND,
            &append_file),
        FILED_OK);
    expect_status(
        "append write uses eof sentinel",
        filed_vfs_write_prepare(&vfs, append_file.handle_id, 1, &decision),
        FILED_OK);
    expect_true("append write sentinel", decision.offset == UINT64_MAX && decision.length == 1);

    expect_status(
        "directory pwrite denied",
        filed_vfs_pwrite_prepare(&vfs, root.handle_id, 0, 1, &decision),
        FILED_ERR_DENIED);
    expect_status("write preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_generation_updates(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_io_decision_t decision;
    uint32_t root_dir_gen;
    uint32_t file_obj_gen;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for generation",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for generation",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP |
                FILED_RIGHT_STAT |
                FILED_RIGHT_GETDENTS |
                FILED_RIGHT_CREATE |
                FILED_RIGHT_REMOVE,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status("generation root getdents", filed_vfs_getdents_prepare(&vfs, root.handle_id, &decision), FILED_OK);
    root_dir_gen = decision.dir_generation;
    expect_true("generation root starts nonzero", root_dir_gen != 0);

    expect_status(
        "generation create child",
        filed_vfs_create_backend_child(
            &vfs,
            root.handle_id,
            400,
            FILED_VNODE_REGULAR,
            "gen.txt",
            FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status("generation root after create", filed_vfs_getdents_prepare(&vfs, root.handle_id, &decision), FILED_OK);
    expect_true("generation create bumps dir", decision.dir_generation > root_dir_gen);
    root_dir_gen = decision.dir_generation;

    memset(&decision, 0, sizeof(decision));
    expect_status("generation file prepare", filed_vfs_pwrite_prepare(&vfs, file.handle_id, 0, 4, &decision), FILED_OK);
    file_obj_gen = decision.object_generation;
    expect_true("generation file starts nonzero", file_obj_gen != 0);

    expect_status("generation note write", filed_vfs_note_write(&vfs, file.handle_id, 0, 4), FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status("generation file after write", filed_vfs_pwrite_prepare(&vfs, file.handle_id, 0, 4, &decision), FILED_OK);
    expect_true("generation write bumps object", decision.object_generation > file_obj_gen);
    file_obj_gen = decision.object_generation;

    expect_status("generation note truncate", filed_vfs_note_truncate(&vfs, file.handle_id, 1), FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status("generation file after truncate", filed_vfs_pwrite_prepare(&vfs, file.handle_id, 0, 4, &decision), FILED_OK);
    expect_true("generation truncate bumps object", decision.object_generation > file_obj_gen);
    file_obj_gen = decision.object_generation;

    expect_status("generation unlink child", filed_vfs_unlink_commit(&vfs, root.handle_id, "gen.txt"), FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status("generation root after unlink", filed_vfs_getdents_prepare(&vfs, root.handle_id, &decision), FILED_OK);
    expect_true("generation unlink bumps dir", decision.dir_generation > root_dir_gen);

    memset(&decision, 0, sizeof(decision));
    expect_status("generation unlinked file alive", filed_vfs_pwrite_prepare(&vfs, file.handle_id, 0, 4, &decision), FILED_OK);
    expect_true("generation unlink bumps object", decision.object_generation > file_obj_gen);

    expect_status("close generation file", filed_vfs_close_handle(&vfs, file.handle_id), FILED_OK);
    expect_status("generation preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_concurrent_open_file_offset(void)
{
    enum {
        THREADS = 8,
        ITERATIONS = 4096,
    };

    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_io_decision_t decision;
    pthread_t threads[THREADS];
    concurrent_offset_args_t args[THREADS];
    unsigned int i;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for concurrent offset",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for concurrent offset",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open concurrent offset child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            50,
            FILED_VNODE_REGULAR,
            "offset.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);

    for (i = 0; i < THREADS; ++i) {
        args[i].vfs = &vfs;
        args[i].handle_id = file.handle_id;
        args[i].iterations = ITERATIONS;
        args[i].status = FILED_ERR_INVALID;
        expect_true(
            "concurrent offset pthread_create",
            pthread_create(&threads[i], NULL, concurrent_offset_worker, &args[i]) == 0);
    }
    for (i = 0; i < THREADS; ++i) {
        expect_true("concurrent offset pthread_join", pthread_join(threads[i], NULL) == 0);
        expect_status("concurrent offset worker status", args[i].status, FILED_OK);
    }

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "concurrent offset final read prepare",
        filed_vfs_read_prepare(&vfs, file.handle_id, 1, &decision),
        FILED_OK);
    expect_true(
        "concurrent offset final value",
        decision.offset == ((uint64_t)THREADS * (uint64_t)ITERATIONS));
    expect_status("concurrent offset preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_concurrent_directory_unlink_parent_lock(void)
{
    enum {
        ENTRIES = 12,
    };

    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t files[ENTRIES];
    char names[ENTRIES][32];
    pthread_t threads[ENTRIES];
    concurrent_unlink_args_t args[ENTRIES];
    unsigned int i;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for concurrent unlink",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for concurrent unlink",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_REMOVE,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);

    for (i = 0; i < ENTRIES; ++i) {
        snprintf(names[i], sizeof(names[i]), "unlink-%02u.txt", i);
        expect_status(
            "open unlink child",
            filed_vfs_open_backend_child(
                &vfs,
                root.handle_id,
                200 + i,
                FILED_VNODE_REGULAR,
                names[i],
                FILED_RIGHT_READ | FILED_RIGHT_STAT,
                0,
                &files[i]),
            FILED_OK);
    }

    for (i = 0; i < ENTRIES; ++i) {
        args[i].vfs = &vfs;
        args[i].parent_handle_id = root.handle_id;
        args[i].name = names[i];
        args[i].status = FILED_ERR_INVALID;
        expect_true(
            "concurrent unlink pthread_create",
            pthread_create(&threads[i], NULL, concurrent_unlink_worker, &args[i]) == 0);
    }
    for (i = 0; i < ENTRIES; ++i) {
        filed_vfs_io_decision_t decision;
        expect_true("concurrent unlink pthread_join", pthread_join(threads[i], NULL) == 0);
        expect_status("concurrent unlink worker status", args[i].status, FILED_OK);
        memset(&decision, 0, sizeof(decision));
        expect_status(
            "concurrent unlink keeps open file alive",
            filed_vfs_pread_prepare(&vfs, files[i].handle_id, 0, 1, &decision),
            FILED_OK);
        expect_true("concurrent unlink backend remains", decision.backend_object == 200 + i);
        expect_status(
            "close concurrent unlinked file",
            filed_vfs_close_handle(&vfs, files[i].handle_id),
            FILED_OK);
    }

    expect_status("concurrent unlink preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_concurrent_cross_directory_rename_ordering(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t dir_a;
    filed_vfs_open_result_t dir_b;
    filed_vfs_open_result_t file_a;
    filed_vfs_open_result_t file_b;
    pthread_t left_thread;
    pthread_t right_thread;
    concurrent_rename_args_t left;
    concurrent_rename_args_t right;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for concurrent rename",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for concurrent rename",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open rename dir a",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            300,
            FILED_VNODE_DIRECTORY,
            "ra",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &dir_a),
        FILED_OK);
    expect_status(
        "open rename dir b",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            301,
            FILED_VNODE_DIRECTORY,
            "rb",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &dir_b),
        FILED_OK);
    expect_status(
        "open left rename child",
        filed_vfs_open_backend_child(
            &vfs,
            dir_a.handle_id,
            302,
            FILED_VNODE_REGULAR,
            "left.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &file_a),
        FILED_OK);
    expect_status(
        "open right rename child",
        filed_vfs_open_backend_child(
            &vfs,
            dir_b.handle_id,
            303,
            FILED_VNODE_REGULAR,
            "right.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &file_b),
        FILED_OK);

    left.vfs = &vfs;
    left.old_parent_handle_id = dir_a.handle_id;
    left.new_parent_handle_id = dir_b.handle_id;
    left.old_name = "left.txt";
    left.new_name = "left.txt";
    left.backend_object = 302;
    left.status = FILED_ERR_INVALID;
    right.vfs = &vfs;
    right.old_parent_handle_id = dir_b.handle_id;
    right.new_parent_handle_id = dir_a.handle_id;
    right.old_name = "right.txt";
    right.new_name = "right.txt";
    right.backend_object = 303;
    right.status = FILED_ERR_INVALID;

    expect_true(
        "concurrent rename left pthread_create",
        pthread_create(&left_thread, NULL, concurrent_rename_worker, &left) == 0);
    expect_true(
        "concurrent rename right pthread_create",
        pthread_create(&right_thread, NULL, concurrent_rename_worker, &right) == 0);
    expect_true("concurrent rename left pthread_join", pthread_join(left_thread, NULL) == 0);
    expect_true("concurrent rename right pthread_join", pthread_join(right_thread, NULL) == 0);
    expect_status("concurrent rename left status", left.status, FILED_OK);
    expect_status("concurrent rename right status", right.status, FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "concurrent rename left file alive",
        filed_vfs_pread_prepare(&vfs, file_a.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("concurrent rename left backend", decision.backend_object == 302);
    memset(&decision, 0, sizeof(decision));
    expect_status(
        "concurrent rename right file alive",
        filed_vfs_pread_prepare(&vfs, file_b.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("concurrent rename right backend", decision.backend_object == 303);
    expect_status("concurrent rename preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_unlink_keeps_open_files_alive(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_io_decision_t decision;
    filed_handle_id_t dup_handle = 0;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for unlink lifetime",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for unlink lifetime",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_REMOVE,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open unlink lifetime child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            60,
            FILED_VNODE_REGULAR,
            "unlink-open.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);
    expect_status(
        "dup unlink lifetime child",
        filed_vfs_dup_handle(&vfs, file.handle_id, 0, &dup_handle),
        FILED_OK);

    expect_status(
        "unlink prepare lifetime child",
        filed_vfs_unlink_prepare(&vfs, root.handle_id, "unlink-open.txt", &decision),
        FILED_OK);
    expect_status(
        "unlink commit lifetime child",
        filed_vfs_unlink_commit(&vfs, root.handle_id, "unlink-open.txt"),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "unlinked source remains readable",
        filed_vfs_pread_prepare(&vfs, file.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("unlinked source backend remains", decision.backend_object == 60);
    memset(&decision, 0, sizeof(decision));
    expect_status(
        "unlinked dup remains readable",
        filed_vfs_pread_prepare(&vfs, dup_handle, 0, 1, &decision),
        FILED_OK);
    expect_true("unlinked dup backend remains", decision.backend_object == 60);

    expect_status("close unlink lifetime source", filed_vfs_close_handle(&vfs, file.handle_id), FILED_OK);
    expect_status("close unlink lifetime dup", filed_vfs_close_handle(&vfs, dup_handle), FILED_OK);
    expect_status("unlink lifetime preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_hardlink_alias_lifetime_and_stat(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t file;
    filed_vfs_open_result_t alias;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_vfs_io_decision_t decision;
    filed_vfs_stat_snapshot_t snapshot;
    filed_vfs_reclaim_result_t reclaim;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for hardlink alias",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for hardlink alias",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_CREATE | FILED_RIGHT_REMOVE | FILED_RIGHT_RENAME | FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "create hardlink cached source",
        filed_vfs_create_backend_child(
            &vfs,
            root.handle_id,
            90,
            FILED_VNODE_REGULAR,
            "file",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
            0,
            &file),
        FILED_OK);
    expect_status(
        "link prepare cached alias",
        filed_vfs_link_prepare(
            &vfs,
            root.handle_id,
            root.handle_id,
            "file",
            "alias",
            &old_parent,
            &new_parent),
        FILED_OK);
    expect_true("link prepare parents", old_parent.backend_object == 11 && new_parent.backend_object == 11);
    expect_status(
        "link commit cached alias",
        filed_vfs_link_commit(
            &vfs,
            root.handle_id,
            90,
            FILED_VNODE_REGULAR,
            "alias",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
            0,
            &alias),
        FILED_OK);

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.mode = 0100644;
    snapshot.size = 7;
    snapshot.blocks = 1;
    snapshot.nlink = 2;
    snapshot.kind = 0100000;
    expect_status("update alias stat", filed_vfs_update_stat_snapshot(&vfs, 90, &snapshot), FILED_OK);
    expect_status("stat source alias", filed_vfs_stat_prepare(&vfs, file.handle_id, &decision), FILED_OK);
    expect_true("stat source nlink", decision.backend_object == 90);
    expect_status("stat hardlink alias", filed_vfs_stat_prepare(&vfs, alias.handle_id, &decision), FILED_OK);
    expect_true("stat alias backend", decision.backend_object == 90);
    memset(&reclaim, 0, sizeof(reclaim));
    expect_status(
        "rename hardlink alias no-op",
        filed_vfs_rename_commit_ex(&vfs, root.handle_id, root.handle_id, "file", "alias", 90, &reclaim),
        FILED_OK);
    expect_true("rename hardlink alias keeps backend", !reclaim.released);
    expect_status("source remains after hardlink rename no-op", filed_vfs_pwrite_prepare(&vfs, file.handle_id, 0, 1, &decision), FILED_OK);
    expect_status("alias remains after hardlink rename no-op", filed_vfs_pwrite_prepare(&vfs, alias.handle_id, 0, 1, &decision), FILED_OK);

    memset(&reclaim, 0, sizeof(reclaim));
    expect_status(
        "unlink source keeps backend via alias",
        filed_vfs_unlink_commit_ex(&vfs, root.handle_id, "file", &reclaim),
        FILED_OK);
    expect_true("unlink source does not release backend", !reclaim.released);
    expect_status("alias remains readable", filed_vfs_pwrite_prepare(&vfs, alias.handle_id, 0, 1, &decision), FILED_OK);
    expect_status("close source hardlink handle", filed_vfs_close_handle(&vfs, file.handle_id), FILED_OK);
    expect_status("close alias hardlink handle", filed_vfs_close_handle(&vfs, alias.handle_id), FILED_OK);
    memset(&reclaim, 0, sizeof(reclaim));
    expect_status(
        "unlink final alias releases backend",
        filed_vfs_unlink_commit_ex(&vfs, root.handle_id, "alias", &reclaim),
        FILED_OK);
    expect_true("unlink final alias release object", reclaim.released && reclaim.backend_object == 90);
    expect_status("hardlink alias preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_rename_replace_keeps_replaced_open_file_alive(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t old_file;
    filed_vfs_open_result_t replaced_file;
    filed_vfs_open_result_t reopened;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for rename lifetime",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for rename lifetime",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open old rename child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            61,
            FILED_VNODE_REGULAR,
            "old-name.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &old_file),
        FILED_OK);
    expect_status(
        "open replaced rename child",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            62,
            FILED_VNODE_REGULAR,
            "new-name.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &replaced_file),
        FILED_OK);
    expect_status(
        "rename prepare replaces child",
        filed_vfs_rename_prepare(
            &vfs,
            root.handle_id,
            root.handle_id,
            "old-name.txt",
            "new-name.txt",
            &old_parent,
            &new_parent),
        FILED_OK);
    expect_status(
        "rename commit replaces child",
        filed_vfs_rename_commit(
            &vfs,
            root.handle_id,
            root.handle_id,
            "old-name.txt",
            "new-name.txt",
            61),
        FILED_OK);

    memset(&decision, 0, sizeof(decision));
    expect_status(
        "replaced open file remains readable",
        filed_vfs_pread_prepare(&vfs, replaced_file.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("replaced open file backend remains", decision.backend_object == 62);

    expect_status(
        "renamed child reopens by new name",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            61,
            FILED_VNODE_REGULAR,
            "new-name.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &reopened),
        FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status(
        "reopened renamed child readable",
        filed_vfs_pread_prepare(&vfs, reopened.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("reopened renamed backend", decision.backend_object == 61);

    expect_status("close old rename file", filed_vfs_close_handle(&vfs, old_file.handle_id), FILED_OK);
    expect_status("close replaced rename file", filed_vfs_close_handle(&vfs, replaced_file.handle_id), FILED_OK);
    expect_status("close reopened rename file", filed_vfs_close_handle(&vfs, reopened.handle_id), FILED_OK);
    expect_status("rename lifetime preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_rename_keeps_open_directory_handle_alive(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_open_result_t dir;
    filed_vfs_open_result_t child;
    filed_vfs_open_result_t child_from_old_handle;
    filed_vfs_open_result_t reopened_dir;
    filed_vfs_open_result_t child_from_reopened_dir;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_vfs_io_decision_t decision;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for directory rename lifetime",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for directory rename lifetime",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);
    expect_status(
        "open directory rename source",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            70,
            FILED_VNODE_DIRECTORY,
            "dir-old",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &dir),
        FILED_OK);
    expect_status(
        "open child before directory rename",
        filed_vfs_open_backend_child(
            &vfs,
            dir.handle_id,
            71,
            FILED_VNODE_REGULAR,
            "child.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &child),
        FILED_OK);
    expect_status(
        "directory rename prepare",
        filed_vfs_rename_prepare(
            &vfs,
            root.handle_id,
            root.handle_id,
            "dir-old",
            "dir-new",
            &old_parent,
            &new_parent),
        FILED_OK);
    expect_status(
        "directory rename commit",
        filed_vfs_rename_commit(
            &vfs,
            root.handle_id,
            root.handle_id,
            "dir-old",
            "dir-new",
            70),
        FILED_OK);

    expect_status(
        "open child through renamed open directory handle",
        filed_vfs_open_backend_child(
            &vfs,
            dir.handle_id,
            71,
            FILED_VNODE_REGULAR,
            "child.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &child_from_old_handle),
        FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status(
        "child from old directory handle remains readable",
        filed_vfs_pread_prepare(&vfs, child_from_old_handle.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("child from old directory handle backend", decision.backend_object == 71);

    expect_status(
        "reopen renamed directory",
        filed_vfs_open_backend_child(
            &vfs,
            root.handle_id,
            70,
            FILED_VNODE_DIRECTORY,
            "dir-new",
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &reopened_dir),
        FILED_OK);
    expect_true("renamed directory reuses vnode", reopened_dir.vnode_id == dir.vnode_id);
    expect_status(
        "open child through reopened renamed directory",
        filed_vfs_open_backend_child(
            &vfs,
            reopened_dir.handle_id,
            71,
            FILED_VNODE_REGULAR,
            "child.txt",
            FILED_RIGHT_READ | FILED_RIGHT_STAT,
            0,
            &child_from_reopened_dir),
        FILED_OK);
    memset(&decision, 0, sizeof(decision));
    expect_status(
        "child from reopened directory remains readable",
        filed_vfs_pread_prepare(&vfs, child_from_reopened_dir.handle_id, 0, 1, &decision),
        FILED_OK);
    expect_true("child from reopened directory backend", decision.backend_object == 71);

    expect_status("close directory rename child", filed_vfs_close_handle(&vfs, child.handle_id), FILED_OK);
    expect_status(
        "close old directory child reopen",
        filed_vfs_close_handle(&vfs, child_from_old_handle.handle_id),
        FILED_OK);
    expect_status(
        "close renamed directory child reopen",
        filed_vfs_close_handle(&vfs, child_from_reopened_dir.handle_id),
        FILED_OK);
    expect_status("close reopened directory", filed_vfs_close_handle(&vfs, reopened_dir.handle_id), FILED_OK);
    expect_status("close renamed old directory handle", filed_vfs_close_handle(&vfs, dir.handle_id), FILED_OK);
    expect_status("directory rename lifetime preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
}

static void test_mutation_component_validation(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    filed_vfs_io_decision_t first;
    filed_vfs_io_decision_t second;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for mutation validation",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for mutation validation",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_REMOVE | FILED_RIGHT_RENAME,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);

    expect_status(
        "unlink rejects dot",
        filed_vfs_unlink_prepare(&vfs, root.handle_id, ".", &first),
        FILED_ERR_INVALID);
    expect_status(
        "unlink rejects dotdot",
        filed_vfs_unlink_prepare(&vfs, root.handle_id, "..", &first),
        FILED_ERR_INVALID);
    expect_status(
        "unlink rejects slash component",
        filed_vfs_unlink_prepare(&vfs, root.handle_id, "a/b", &first),
        FILED_ERR_INVALID);
    expect_status(
        "rename rejects slash component",
        filed_vfs_rename_prepare(
            &vfs,
            root.handle_id,
            root.handle_id,
            "a/b",
            "c",
            &first,
            &second),
        FILED_ERR_INVALID);
    expect_status("mutation validation preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
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

static bool test_backend_object_evictable(void *context, filed_backend_object_id_t backend_object)
{
    (void)context;
    (void)backend_object;
    return true;
}

static void test_linked_vnode_lru_reuses_capacity(void)
{
    filed_vfs_t vfs;
    filed_mount_id_t root_mount = 0;
    filed_vfs_open_result_t root;
    unsigned int evictions = 0;

    filed_vfs_init(&vfs);
    expect_status(
        "mount for linked vnode lru",
        filed_vfs_mount_root(&vfs, FILED_FS_SYNTHETIC, 7, 11, &root_mount),
        FILED_OK);
    expect_status(
        "open root for linked vnode lru",
        filed_vfs_open_root(
            &vfs,
            root_mount,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
            FILED_OPEN_DIRECTORY,
            &root),
        FILED_OK);

    for (unsigned int i = 0; i < 160; ++i) {
        char name[32];
        filed_vfs_open_result_t file;
        filed_vfs_reclaim_result_t reclaim;
        snprintf(name, sizeof(name), "header-%03u.h", i);
        expect_status(
            "open distinct linked vnode",
            filed_vfs_open_backend_child(
                &vfs,
                root.handle_id,
                1000u + i,
                FILED_VNODE_REGULAR,
                name,
                FILED_RIGHT_READ | FILED_RIGHT_STAT,
                0,
                &file),
            FILED_OK);
        expect_status(
            "close distinct linked vnode",
            filed_vfs_close_handle(&vfs, file.handle_id),
            FILED_OK);
        memset(&reclaim, 0, sizeof(reclaim));
        expect_status(
            "evict distinct linked vnode",
            filed_vfs_evict_lru_unused_linked(
                &vfs,
                32,
                test_backend_object_evictable,
                NULL,
                &reclaim),
            FILED_OK);
        if (reclaim.released) {
            ++evictions;
        }
    }
    expect_true("linked vnode lru evicted after 100+ opens", evictions >= 128);
    expect_status("linked vnode lru preserves invariant", filed_vfs_check_basic(&vfs), FILED_OK);
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
    test_write_pwrite_and_fsync();
    test_generation_updates();
    test_concurrent_open_file_offset();
    test_concurrent_directory_unlink_parent_lock();
    test_concurrent_cross_directory_rename_ordering();
    test_unlink_keeps_open_files_alive();
    test_hardlink_alias_lifetime_and_stat();
    test_rename_replace_keeps_replaced_open_file_alive();
    test_rename_keeps_open_directory_handle_alive();
    test_mutation_component_validation();
    test_rights_and_flags();
    test_invalid_arguments();
    test_linked_vnode_lru_reuses_capacity();

    if (failures != 0) {
        printf("filed vfs tests failed: %d\n", failures);
        return 1;
    }

    printf("filed vfs tests passed\n");
    return 0;
}
