#include "../userland/personality/linux/runtime/lpr_process/credentials.c"
#include "../userland/personality/linux/runtime/lpr_vfs/ops.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/lpr_supervisor/src/credentials.h"

lpr_state_t lpr_state;
static _Alignas(8) char wire[PACHA_SERVICE_PAGE_BYTES];
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
int lpr_create_standalone_wire_page(void **page) { *page = wire; return 16; }
void lpr_destroy_standalone_wire_page(int fd, void *page) { assert(fd == 16 && page == wire); }
void *lpr_supervisor_payload(void *page) { return (char *)page + PACHA_SERVICE_HEADER_BYTES; }
int64_t lpr_supervisor_call(uint32_t op, int fd, void *page, uint32_t size, int transfer, uint64_t *result)
{
    assert(op == LPRS_OP_PROCESS_CREDENTIALS && fd == 16 && page == wire &&
        size == sizeof(lprs_credential_request_t) && transfer == -1 && !result);
    lprs_credential_request_t *request = lpr_supervisor_payload(page);
    lprs_credentials_t next;
    int status = lprs_credentials_apply(&lpr_state.process.credentials, 0, request, &next);
    if (!status) { lpr_state.process.credentials = next; request->credentials = next; }
    return status;
}
void lpr_linux_process_state_init(void) {}
int lpr_user_range_plausible(uint64_t ptr, uint64_t bytes)
{ return ptr >= 4096 && ptr < (1ull << 47) && bytes < (1ull << 47) - ptr; }

int main(void)
{
    assert(lpr_linux_getuid() == -LPR_LINUX_ENOSYS);
    lpr_supervisor_enabled = 1;
    const lprs_credentials_t expected = { .uid=1001, .euid=1002, .suid=1003,
        .gid=2001, .egid=2002, .sgid=2003, .group_count=2, .groups={3001,3002} };
    lpr_state.process.credentials = expected;
    assert(lpr_linux_getuid() == 1001 && lpr_linux_geteuid() == 1002);
    assert(lpr_linux_getgid() == 2001 && lpr_linux_getegid() == 2002);
    uint32_t groups[2] = {0};
    assert(lpr_linux_getgroups(0, 0) == 2);
    assert(lpr_linux_getgroups(1, (uintptr_t)groups) == -LPR_LINUX_EINVAL);
    assert(lpr_linux_getgroups(-1, (uintptr_t)groups) == -LPR_LINUX_EINVAL);
    assert(lpr_linux_getgroups(2, 0) == -LPR_LINUX_EFAULT);
    assert(lpr_linux_getgroups(2, (uintptr_t)groups) == 2 && groups[0] == 3001 && groups[1] == 3002);
    assert(lpr_linux_setgroups(2, (uintptr_t)groups) == -LPR_LINUX_EPERM);
    uint32_t r, e, s;
    assert(!lpr_linux_getresuid((uintptr_t)&r, (uintptr_t)&e, (uintptr_t)&s));
    assert(r == 1001 && e == 1002 && s == 1003);
    assert(!lpr_linux_getresgid((uintptr_t)&r, (uintptr_t)&e, (uintptr_t)&s));
    assert(r == 2001 && e == 2002 && s == 2003);
    assert(lpr_linux_getresuid(0, (uintptr_t)&e, (uintptr_t)&s) == -LPR_LINUX_EFAULT);
    assert(lpr_linux_getresgid((uintptr_t)&r, (uintptr_t)&e, UINT64_MAX) == -LPR_LINUX_EFAULT);
    assert(!lpr_linux_setresuid(-1, -1, -1));
    assert(!lpr_linux_setresuid(1001, 1002, 1003));
    assert(!lpr_linux_setresgid(2001, 2002, 2003));
    assert(lpr_linux_setuid(0) == -LPR_LINUX_EPERM);
    assert(lpr_linux_setgid(0) == -LPR_LINUX_EPERM);
    assert(lpr_linux_setuid(UINT32_MAX) == -LPR_LINUX_EINVAL);
    assert(lpr_linux_setresuid(-1, 0, -1) == -LPR_LINUX_EPERM);
    assert(lpr_linux_setresgid(-1, -1, 0) == -LPR_LINUX_EPERM);
    assert(!memcmp(&expected, &lpr_state.process.credentials, sizeof(expected)));
    memset(&lpr_state.process.credentials, 0, sizeof(expected));
    assert(!lpr_linux_setuid(0) && !lpr_linux_setgid(0));
    assert(lpr_linux_getgroups(0, 0) == 0);
    assert(lpr_linux_setgroups(0, 0) == -LPR_LINUX_EPERM);
    assert(lpr_linux_setuid(1) == -LPR_LINUX_EPERM);
    lprs_credential_request_t req = { .operation = LPRS_CREDENTIAL_RESUID, .ids = {1003,1001,1002} };
    lprs_credentials_t next;
    assert(!lprs_credentials_apply(&expected, 0, &req, &next));
    assert(next.uid == 1003 && next.euid == 1001 && next.suid == 1002);
    req.operation = LPRS_CREDENTIAL_REUID; req.ids[0] = UINT32_MAX; req.ids[1] = 1003;
    assert(!lprs_credentials_apply(&expected, 0, &req, &next));
    assert(next.uid == 1001 && next.euid == 1003 && next.suid == 1003);
    req.ids[0] = 1003;
    assert(lprs_credentials_apply(&expected, 0, &req, &next) == -1);
    req.operation = LPRS_CREDENTIAL_UID; req.ids[0] = 81;
    assert(!lprs_credentials_apply(&expected, LPRS_CREDENTIAL_SETUID, &req, &next));
    assert(next.uid == 81 && next.euid == 81 && next.suid == 81 && next.gid == expected.gid);
    assert(lprs_credentials_apply(&expected, LPRS_CREDENTIAL_SETGID, &req, &next) == -1);
    req.operation = LPRS_CREDENTIAL_GROUPS;
    req.credentials = (lprs_credentials_t){ .group_count = 2, .groups = {1000,100} };
    assert(!lprs_credentials_apply(&expected, LPRS_CREDENTIAL_SETGID, &req, &next));
    assert(next.group_count == 2 && next.groups[0] == 100 && next.groups[1] == 1000);
    req.credentials.group_count = LPRS_MAX_GROUPS + 1;
    assert(lprs_credentials_apply(&expected, 3, &req, &next) == -22);
    lpr_state.process.filed_rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_READ;
    assert(lpr_open_rights(LPR_LINUX_O_RDONLY) == lpr_state.process.filed_rights);
    assert((lpr_open_rights(LPR_LINUX_O_WRONLY | LPR_LINUX_O_CREAT) &
        (FILED_RIGHT_CREATE | FILED_RIGHT_WRITE)) == (FILED_RIGHT_CREATE | FILED_RIGHT_WRITE));
    puts("credentials: manager RPC getters/setters, distinct IDs, denied forgery, no root fallback passed");
}
