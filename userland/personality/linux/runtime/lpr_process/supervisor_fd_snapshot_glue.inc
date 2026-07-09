static int lpr_supervisor_snapshot_count(void *ctx, uint64_t *out_count)
{
    (void)ctx;
    return lpr_count_exec_local_fds(out_count);
}

static int lpr_supervisor_snapshot_next(
    void *ctx,
    uint64_t *cursor,
    lprs_v2_fd_desc_t *out,
    int *out_has)
{
    (void)ctx;
    if (cursor == 0 || out == 0 || out_has == 0) {
        return -LPR_LINUX_EINVAL;
    }
    *out_has = 0;
    for (uint64_t fd = *cursor; fd < lpr_fd_table_capacity; fd += 1) {
        int preserve = 0;
        const int status = lpr_exec_local_fd_preserve(fd, &preserve);
        if (status != 0) {
            return status;
        }
        if (!preserve) {
            continue;
        }
        filed_v2_exec_lpr_fd_t desc;
        lpr_write_exec_local_fd_desc(&desc, fd);
        lpr_memset(out, 0, sizeof(*out));
        out->fd = desc.fd;
        out->kind = desc.kind;
        out->fd_flags = lpr_control_fd_flags_to_lprs(fd);
        out->status_flags = lpr_control_status_flags_to_lprs(fd);
        out->handle = desc.handle;
        out->offset_or_counter = desc.offset_or_counter;
        *cursor = fd + 1u;
        *out_has = 1;
        return 0;
    }
    *cursor = lpr_fd_table_capacity;
    return 0;
}

static int lpr_supervisor_snapshot_install(void *ctx, const lprs_v2_fd_desc_t *in)
{
    (void)ctx;
    if (in == 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_bootstrap_fd_t desc;
    lpr_memset(&desc, 0, sizeof(desc));
    desc.fd = in->fd;
    desc.kind = in->kind;
    desc.flags =
        lpr_lprs_fd_flags_to_linux(in->fd_flags) |
        lpr_lprs_status_flags_to_linux(in->status_flags);
    desc.handle = in->handle;
    desc.offset_or_counter = in->offset_or_counter;
    return lpr_install_local_fd_descs(&desc, 1) ? 0 : -LPR_LINUX_EIO;
}

static void lpr_supervisor_snapshot_ops(lpr_supervisor_fd_snapshot_ops_t *ops)
{
    lpr_memset(ops, 0, sizeof(*ops));
    ops->request_counter = &lpr_request_id;
    ops->status_to_errno = lpr_pacha_status_to_errno;
    ops->create_page = lpr_create_standalone_wire_page;
    ops->destroy_page = lpr_destroy_standalone_wire_page;
    ops->count_fds = lpr_supervisor_snapshot_count;
    ops->next_fd = lpr_supervisor_snapshot_next;
    ops->install_fd = lpr_supervisor_snapshot_install;
}
