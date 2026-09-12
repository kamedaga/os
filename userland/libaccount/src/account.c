#include "pacha/account.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void pacha_accounts_destroy(struct pacha_accounts *db)
{
    free(db->passwd_text); free(db->group_text);
    free(db->users); free(db->groups);
    *db = (struct pacha_accounts){0};
}

#define LOOKUP(function, type, list, count, argument, match) \
const struct type *function(const struct pacha_accounts *db, argument) { \
    for (size_t i = 0; i < db->count; i++) if (match) return &db->list[i]; \
    return NULL; \
}
LOOKUP(pacha_account_by_name, pacha_account, users, user_count, const char *name, name && !strcmp(db->users[i].name, name))
LOOKUP(pacha_account_by_uid, pacha_account, users, user_count, uint32_t uid, db->users[i].uid == uid)
LOOKUP(pacha_group_by_name, pacha_account_group, groups, group_count, const char *name, name && !strcmp(db->groups[i].name, name))
LOOKUP(pacha_group_by_gid, pacha_account_group, groups, group_count, uint32_t gid, db->groups[i].gid == gid)
#undef LOOKUP

static int identifier(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '$')) return 0;
    }
    return 1;
}

static int number(const char *s, uint32_t *out)
{
    uint32_t n = 0;
    if (!*s) return EINVAL;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - 1u - (unsigned)(*s - '0')) / 10u) return EINVAL;
        n = n * 10u + (unsigned)(*s - '0');
    }
    *out = n;
    return 0;
}

static int fields(char *line, char **out, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        out[i] = line;
        char *end = strchr(line, ':');
        if (i + 1 == count) return end ? EINVAL : 0;
        if (!end) return EINVAL;
        *end = 0; line = end + 1;
    }
    return EINVAL;
}

static char *next_line(char **cursor)
{
    while (**cursor) {
        char *line = *cursor, *end = strchr(line, '\n');
        if (end) { *end = 0; *cursor = end + 1; }
        else *cursor = line + strlen(line);
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r') line[--length] = 0;
        if (length && *line != '#') return line;
    }
    return NULL;
}

static int member(const char *list, const char *name)
{
    size_t length = strlen(name);
    while (*list) {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        if (length == n && !memcmp(name, list, n)) return 1;
        if (!end) break;
        list = end + 1;
    }
    return 0;
}

int pacha_account_groups(const struct pacha_accounts *db, const char *name, uint32_t *out, size_t *count)
{
    if (!count || (*count && !out)) return EINVAL;
    const struct pacha_account *user = pacha_account_by_name(db, name);
    if (!user) return ENOENT;
    size_t needed = 0, capacity = *count;
    for (size_t i = 0; i < db->group_count; i++)
        if (db->groups[i].gid != user->gid && member(db->groups[i].members, name)) needed++;
    *count = needed;
    if (needed > capacity) return ERANGE;
    needed = 0;
    for (size_t i = 0; i < db->group_count; i++)
        if (db->groups[i].gid != user->gid && member(db->groups[i].members, name)) out[needed++] = db->groups[i].gid;
    return 0;
}

int pacha_accounts_parse(struct pacha_accounts *db, const char *passwd, size_t plen, const char *group, size_t glen)
{
    if (!passwd || !group || plen == SIZE_MAX || glen == SIZE_MAX ||
        memchr(passwd, 0, plen) || memchr(group, 0, glen)) return EINVAL;
    struct pacha_accounts next = {0};
    int status = ENOMEM;
    next.passwd_text = malloc(plen + 1);
    next.group_text = malloc(glen + 1);
    if (!next.passwd_text || !next.group_text) goto fail;
    memcpy(next.passwd_text, passwd, plen); next.passwd_text[plen] = 0;
    memcpy(next.group_text, group, glen); next.group_text[glen] = 0;
    char *cursor = next.group_text, *line;
    while ((line = next_line(&cursor))) {
        char *f[4]; uint32_t gid;
        status = EINVAL;
        if (fields(line, f, 4) || !identifier(f[0]) || number(f[2], &gid) ||
            pacha_group_by_name(&next, f[0]) || pacha_group_by_gid(&next, gid)) goto fail;
        status = ENOMEM;
        void *entries = realloc(next.groups, (next.group_count + 1) * sizeof(*next.groups));
        if (!entries) goto fail;
        next.groups = entries;
        next.groups[next.group_count++] = (struct pacha_account_group){f[0], f[3], gid};
    }
    cursor = next.passwd_text;
    while ((line = next_line(&cursor))) {
        char *f[7]; uint32_t uid, gid;
        status = EINVAL;
        if (fields(line, f, 7) || !identifier(f[0]) || number(f[2], &uid) || number(f[3], &gid) ||
            f[5][0] != '/' || (f[6][0] && f[6][0] != '/') ||
            !pacha_group_by_gid(&next, gid) || pacha_account_by_name(&next, f[0]) ||
            pacha_account_by_uid(&next, uid)) goto fail;
        status = ENOMEM;
        void *entries = realloc(next.users, (next.user_count + 1) * sizeof(*next.users));
        if (!entries) goto fail;
        next.users = entries;
        next.users[next.user_count++] = (struct pacha_account){f[0], f[5], f[6], uid, gid};
    }
    status = EINVAL;
    for (size_t i = 0; i < next.group_count; i++) {
        char *p = next.groups[i].members;
        while (*p) {
            char *end = strchr(p, ',');
            if (end) *end = 0;
            int valid = identifier(p) && pacha_account_by_name(&next, p);
            if (end) *end = ',';
            if (!valid || (end && !end[1])) goto fail;
            if (!end) break;
            p = end + 1;
        }
    }
    pacha_accounts_destroy(db);
    *db = next;
    return 0;
fail:
    pacha_accounts_destroy(&next);
    return status;
}
