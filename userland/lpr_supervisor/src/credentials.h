#pragma once
#include "lpr_supervisor/ipc_protocol.h"

/* Pure policy: caller authentication and publication belong to the manager.
 * Rights are explicitly delegated at launch, never inferred from UID zero.
 * They authorize identity changes, not acquisition of any native capability. */
static int lprs_credentials_apply(const lprs_credentials_t *old, uint32_t rights,
    const lprs_credential_request_t *request, lprs_credentials_t *next)
{
    *next = *old;
    unsigned op = request->operation;
    if (op == LPRS_CREDENTIAL_READ) return 0;
    if (op == LPRS_CREDENTIAL_GROUPS) {
        if (request->credentials.reserved || request->credentials.group_count > LPRS_MAX_GROUPS)
            return -22;
        if (!(rights & LPRS_CREDENTIAL_SETGID)) return -1;
        next->group_count = request->credentials.group_count;
        for (unsigned i = 0; i < LPRS_MAX_GROUPS; i++) {
            uint32_t gid = i < next->group_count ? request->credentials.groups[i] : 0;
            if (gid == UINT32_MAX) return -22;
            next->groups[i] = gid;
        }
        /* Linux getgroups exposes a sorted supplementary set. */
        for (unsigned i = 1; i < next->group_count; i++) {
            uint32_t value = next->groups[i];
            unsigned j = i;
            while (j && next->groups[j - 1] > value) { next->groups[j] = next->groups[j - 1]; j--; }
            next->groups[j] = value;
        }
        return 0;
    }
    if (op < LPRS_CREDENTIAL_UID || op > LPRS_CREDENTIAL_RESGID) return -22;
    int group = !(op & 1u);
    int privileged = (rights & (group ? LPRS_CREDENTIAL_SETGID : LPRS_CREDENTIAL_SETUID)) != 0;
    uint32_t r = group ? old->gid : old->uid;
    uint32_t e = group ? old->egid : old->euid;
    uint32_t s = group ? old->sgid : old->suid;
    uint32_t nr = r, ne = e, ns = s;
    uint32_t a = request->ids[0], b = request->ids[1], c = request->ids[2];
    if (op <= LPRS_CREDENTIAL_GID) {
        if (a == UINT32_MAX) return -22;
        if (!privileged && a != r && a != s) return -1;
        ne = a;
        if (privileged) nr = ns = a;
    } else if (op <= LPRS_CREDENTIAL_REGID) {
        if (!privileged && ((a != UINT32_MAX && a != r && a != e) ||
            (b != UINT32_MAX && b != r && b != e && b != s))) return -1;
        if (a != UINT32_MAX) nr = a;
        if (b != UINT32_MAX) ne = b;
        if (a != UINT32_MAX || (b != UINT32_MAX && b != r)) ns = ne;
    } else {
        const uint32_t ids[] = {a,b,c};
        for (unsigned i = 0; i < 3; i++)
            if (!privileged && ids[i] != UINT32_MAX && ids[i] != r && ids[i] != e && ids[i] != s)
                return -1;
        if (a != UINT32_MAX) nr = a;
        if (b != UINT32_MAX) ne = b;
        if (c != UINT32_MAX) ns = c;
    }
    if (group) { next->gid = nr; next->egid = ne; next->sgid = ns; }
    else { next->uid = nr; next->euid = ne; next->suid = ns; }
    return 0;
}
