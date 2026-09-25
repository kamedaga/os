#include "pacha/account.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char passwd[] = "# common users\r\nroot:x:0:0:root:/root:/bin/bash\n"
    "user:x:1000:1000:User:/home/user:/bin/bash\nsvc:x:81:81:Service:/run/svc:/sbin/nologin";
static const char group[] = "root:x:0:\nuser:x:1000:user\nsvc:x:81:\n"
    "shared:x:100:user,svc,user\nsvc-extra:x:101:svc\n";

static void reject(struct pacha_accounts *db, const char *pw, const char *gr)
{
    const struct pacha_account *before = pacha_account_by_name(db, "svc");
    assert(pacha_accounts_parse(db, pw, strlen(pw), gr, strlen(gr)) == EINVAL);
    assert(pacha_account_by_name(db, "svc") == before);
}

int main(int argc, char **argv)
{
    struct pacha_accounts db = {0};
    assert(!pacha_accounts_parse(&db, passwd, sizeof(passwd)-1, group, sizeof(group)-1));
    assert(db.user_count == 3 && db.group_count == 5);
    assert(pacha_account_by_name(&db, "user") == pacha_account_by_uid(&db, 1000));
    const struct pacha_account *svc = pacha_account_by_uid(&db, 81);
    assert(svc && !strcmp(svc->name, "svc") && !strcmp(svc->shell, "/sbin/nologin"));
    assert(pacha_group_by_name(&db, "shared") == pacha_group_by_gid(&db, 100));
    assert(!pacha_account_by_name(&db, "missing") && !pacha_account_by_uid(&db, UINT32_MAX));
    uint32_t ids[3] = {999,999,999}; size_t n = 0;
    assert(pacha_account_groups(&db, "svc", NULL, &n) == ERANGE && n == 2);
    n = 1;
    assert(pacha_account_groups(&db, "svc", ids, &n) == ERANGE && n == 2 && ids[0] == 999);
    n = 3;
    assert(!pacha_account_groups(&db, "svc", ids, &n) && n == 2 && ids[0] == 100 && ids[1] == 101);
    n = 3;
    assert(!pacha_account_groups(&db, "user", ids, &n) && n == 1 && ids[0] == 100);
    n = 0;
    assert(!pacha_account_groups(&db, "root", NULL, &n) && !n);
    assert(pacha_account_groups(&db, "missing", NULL, &n) == ENOENT);
    reject(&db, "root:x:0:0:root:/root:/bin/bash\nother:x:0:0:other:/root:/bin/bash\n", group);
    reject(&db, "root:x:-1:0:root:/root:/bin/bash\n", group);
    reject(&db, "root:x:4294967295:0:root:/root:/bin/bash\n", group);
    reject(&db, "root:x:42949672960:0:root:/root:/bin/bash\n", group);
    reject(&db, "root:x:0:7:root:/root:/bin/bash\n", group);
    reject(&db, passwd, "root:x:0:\nuser:x:1000:\nsvc:x:81:\ndup:x:81:\n");
    reject(&db, passwd, "root:x:0:\nuser:x:1000:\nsvc:x:81:missing\n");
    reject(&db, passwd, "root:x:0:\nuser:x:1000:\nsvc:x:81:svc,\n");
    reject(&db, "broken:line\n", group);
    pacha_accounts_destroy(&db);
    /* Validate the actual canonical inputs using the same resolver as boot. */
    if (argc == 3) {
        char *text[2]; size_t lengths[2];
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(argv[i+1], "rb"); assert(f);
            assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); assert(length >= 0);
            rewind(f); text[i] = malloc((size_t)length + 1); assert(text[i]);
            lengths[i] = fread(text[i], 1, (size_t)length, f); assert(lengths[i] == (size_t)length);
            assert(!fclose(f));
        }
        assert(!pacha_accounts_parse(&db, text[0], lengths[0], text[1], lengths[1]));
        assert(pacha_account_by_name(&db, "root") && pacha_account_by_name(&db, "user") && pacha_account_by_name(&db, "messagebus"));
        free(text[0]); free(text[1]); pacha_accounts_destroy(&db);
    }
    puts("ACCOUNTS_UNIT=OK canonical-files humans-services names-ids supplementary strict-validation atomic-failure");
}
