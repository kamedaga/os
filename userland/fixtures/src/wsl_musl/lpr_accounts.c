#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
/* Compare the supervisor's shared resolver with the guest's unmodified libc. */
#include "../../../libaccount/src/account.c"

static int load(const char *path, char **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *text = malloc(1024 * 1024); if (!text) { fclose(f); return -1; }
    *size = fread(text, 1, 1024 * 1024, f);
    int failed = ferror(f) || !feof(f);
    fclose(f); if (failed) { free(text); return -1; }
    *out = text; return 0;
}

static int check(void)
{
    char *pw = NULL, *gr = NULL; size_t plen = 0, glen = 0;
    struct pacha_accounts db = {0};
    if (load("/etc/passwd", &pw, &plen) || load("/etc/group", &gr, &glen) ||
        pacha_accounts_parse(&db, pw, plen, gr, glen)) return 1;
    free(pw); free(gr);
    if (!pacha_account_by_name(&db, "root") || !pacha_account_by_name(&db, "user") ||
        !pacha_account_by_name(&db, "messagebus")) return 2;
    for (size_t i = 0; i < db.user_count; i++) {
        const struct pacha_account *account = &db.users[i];
        struct passwd entry, *result = NULL; char buffer[2048];
        if (getpwnam_r(account->name, &entry, buffer, sizeof(buffer), &result) || !result ||
            entry.pw_uid != account->uid || entry.pw_gid != account->gid ||
            strcmp(entry.pw_dir, account->home) || strcmp(entry.pw_shell, account->shell)) return 3;
        if (getpwuid_r(account->uid, &entry, buffer, sizeof(buffer), &result) || !result ||
            strcmp(entry.pw_name, account->name)) return 4;
        uint32_t expected[64]; size_t count = 64;
        if (pacha_account_groups(&db, account->name, expected, &count)) return 5;
        gid_t actual[65]; int capacity = 65;
        if (getgrouplist(account->name, account->gid, actual, &capacity) < 0 ||
            capacity != (int)count + 1 || actual[0] != account->gid) return 6;
        for (size_t j = 0; j < count; j++) if (actual[j+1] != expected[j]) return 7;
    }
    for (size_t i = 0; i < db.group_count; i++) {
        const struct pacha_account_group *group = &db.groups[i];
        struct group entry, *result = NULL; char buffer[2048];
        if (getgrnam_r(group->name, &entry, buffer, sizeof(buffer), &result) || !result || entry.gr_gid != group->gid) return 8;
        if (getgrgid_r(group->gid, &entry, buffer, sizeof(buffer), &result) || !result || strcmp(entry.gr_name, group->name)) return 9;
    }
    const struct pacha_account *self = pacha_account_by_uid(&db, getuid());
    if (!self || getgid() != self->gid) return 10;
    uint32_t expected[64]; size_t count = 64; gid_t actual[64];
    if (pacha_account_groups(&db, self->name, expected, &count) || getgroups(0, NULL) != (int)count ||
        getgroups(64, actual) != (int)count || memcmp(actual, expected, count * sizeof(*actual))) return 11;
    errno = 0;
    if (syscall(SYS_getgroups, -1, NULL) != -1 || errno != EINVAL) return 12;
    gid_t forged = 7777; errno = 0;
    if (setgroups(1, &forged) != -1 || errno != EPERM || getgroups(0, NULL) != (int)count) return 13;
    pacha_accounts_destroy(&db);
    return 0;
}

int main(int argc, char **argv)
{
    int status = check();
    if (status) { fprintf(stderr, "LPR_ACCOUNTS=FAIL stage=%d errno=%d\n", status, errno); return status; }
    if (argc == 2 && !strcmp(argv[1], "--exec")) { puts("LPR_ACCOUNTS_EXEC=OK"); return 0; }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) return 20;
    if (!child) {
        status = check(); if (status) _exit(status);
        execl(argv[0], argv[0], "--exec", NULL); _exit(21);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 22;
    puts("LPR_ACCOUNTS=OK passwd-group names-ids humans-services supplementary process-groups fork exec no-escalation");
    return 0;
}
