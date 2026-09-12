#include "../lpr_filed_internal.h"

static int64_t credential_call(lprs_credential_request_t *request)
{
    lpr_linux_process_state_init();
    if (!lpr_supervisor_enabled) return -LPR_LINUX_ENOSYS;
    void *page = 0;
    int fd = lpr_create_standalone_wire_page(&page);
    if (fd < 0) return fd;
    request->token = lpr_supervisor_token;
    void *payload = lpr_supervisor_payload(page);
    lpr_memcpy(payload, request, sizeof(*request));
    int64_t status = lpr_supervisor_call(LPRS_OP_PROCESS_CREDENTIALS,
        fd, page, sizeof(*request), -1, 0);
    if (!status) lpr_memcpy(request, payload, sizeof(*request));
    lpr_destroy_standalone_wire_page(fd, page);
    return status;
}

#define GET_ID(name, field) \
int64_t lpr_linux_##name(void) { \
    lprs_credential_request_t request = {0}; \
    int64_t status = credential_call(&request); \
    return status ? status : (int64_t)request.credentials.field; \
}
GET_ID(getuid, uid)
GET_ID(geteuid, euid)
GET_ID(getgid, gid)
GET_ID(getegid, egid)
#undef GET_ID

int64_t lpr_linux_getgroups(uint64_t count, uint64_t groups)
{
    int32_t capacity = (int32_t)count;
    if (capacity < 0) return -LPR_LINUX_EINVAL;
    lprs_credential_request_t request = {0};
    int64_t status = credential_call(&request);
    if (status) return status;
    const lprs_credentials_t *c = &request.credentials;
    if (c->reserved || c->group_count > LPRS_MAX_GROUPS) return -LPR_LINUX_EIO;
    if (!capacity) return c->group_count;
    if ((uint32_t)capacity < c->group_count) return -LPR_LINUX_EINVAL;
    if (c->group_count && !lpr_user_range_plausible(groups, c->group_count * sizeof(uint32_t)))
        return -LPR_LINUX_EFAULT;
    for (uint32_t i = 0; i < c->group_count; i++) ((uint32_t *)(uintptr_t)groups)[i] = c->groups[i];
    return c->group_count;
}

int64_t lpr_linux_setgroups(uint64_t count, uint64_t groups)
{
    if (count > LPRS_MAX_GROUPS) return -LPR_LINUX_EINVAL;
    if (count && !lpr_user_range_plausible(groups, count * sizeof(uint32_t))) return -LPR_LINUX_EFAULT;
    lprs_credential_request_t request = { .operation = LPRS_CREDENTIAL_GROUPS };
    request.credentials.group_count = (uint32_t)count;
    if (count) lpr_memcpy(request.credentials.groups, (void *)(uintptr_t)groups, count * sizeof(uint32_t));
    return credential_call(&request);
}

static int64_t get_res(int group, uint64_t real, uint64_t effective, uint64_t saved)
{
    if (!lpr_user_range_plausible(real, sizeof(uint32_t)) ||
        !lpr_user_range_plausible(effective, sizeof(uint32_t)) ||
        !lpr_user_range_plausible(saved, sizeof(uint32_t))) return -LPR_LINUX_EFAULT;
    lprs_credential_request_t request = {0};
    int64_t status = credential_call(&request);
    if (status) return status;
    const lprs_credentials_t *c = &request.credentials;
    *(uint32_t *)(uintptr_t)real = group ? c->gid : c->uid;
    *(uint32_t *)(uintptr_t)effective = group ? c->egid : c->euid;
    *(uint32_t *)(uintptr_t)saved = group ? c->sgid : c->suid;
    return 0;
}

static int64_t set_ids(unsigned operation, uint64_t real, uint64_t effective, uint64_t saved)
{
    lprs_credential_request_t request = { .operation = operation,
        .ids = {(uint32_t)real, (uint32_t)effective, (uint32_t)saved} };
    return credential_call(&request);
}

int64_t lpr_linux_getresuid(uint64_t r, uint64_t e, uint64_t s) { return get_res(0, r, e, s); }
int64_t lpr_linux_getresgid(uint64_t r, uint64_t e, uint64_t s) { return get_res(1, r, e, s); }
int64_t lpr_linux_setresuid(uint64_t r, uint64_t e, uint64_t s) { return set_ids(LPRS_CREDENTIAL_RESUID, r,e,s); }
int64_t lpr_linux_setresgid(uint64_t r, uint64_t e, uint64_t s) { return set_ids(LPRS_CREDENTIAL_RESGID, r,e,s); }
int64_t lpr_linux_setreuid(uint64_t r, uint64_t e) { return set_ids(LPRS_CREDENTIAL_REUID, r,e,0); }
int64_t lpr_linux_setregid(uint64_t r, uint64_t e) { return set_ids(LPRS_CREDENTIAL_REGID, r,e,0); }
int64_t lpr_linux_setuid(uint64_t id) { return set_ids(LPRS_CREDENTIAL_UID, id,0,0); }
int64_t lpr_linux_setgid(uint64_t id) { return set_ids(LPRS_CREDENTIAL_GID, id,0,0); }
