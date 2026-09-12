#pragma once
#include <stddef.h>
#include <stdint.h>

/* Account data is naming/compatibility information, never capability authority.
 * /etc/passwd and /etc/group are the sole source, for humans and services alike.
 * A database is an immutable snapshot; replacing it does not change credentials
 * of already running processes. No password verification or NSS daemon here. */
struct pacha_account {
    char *name, *home, *shell;
    uint32_t uid, gid;
};
struct pacha_account_group {
    char *name, *members;
    uint32_t gid;
};
struct pacha_accounts {
    char *passwd_text, *group_text;
    struct pacha_account *users;
    struct pacha_account_group *groups;
    size_t user_count, group_count;
};

/* Zero-initialize before first use. Failure leaves the previous snapshot intact.
 * Returns zero or a positive errno. IDs must be unique and cannot be UINT32_MAX;
 * primary groups and explicitly named members must exist. */
int pacha_accounts_parse(struct pacha_accounts *, const char *, size_t, const char *, size_t);
void pacha_accounts_destroy(struct pacha_accounts *);
const struct pacha_account *pacha_account_by_name(const struct pacha_accounts *, const char *);
const struct pacha_account *pacha_account_by_uid(const struct pacha_accounts *, uint32_t);
const struct pacha_account_group *pacha_group_by_name(const struct pacha_accounts *, const char *);
const struct pacha_account_group *pacha_group_by_gid(const struct pacha_accounts *, uint32_t);
/* Supplementary groups only; excludes the primary group. *count is capacity on
 * entry and required size on return. ERANGE never returns a truncated list. */
int pacha_account_groups(const struct pacha_accounts *, const char *, uint32_t *, size_t *count);
