#pragma once

/* Trusted boot configuration. Account resolution and credential authority are
 * handled by lpr_supervisor; client FDs are explicit filed launch grants.
 * Services need not start as root or call setuid in an application wrapper.
 * No credential-changing authority is granted unless named here. It remains
 * independent of UID/GID, as do the native capabilities inherited by children. */
typedef struct seed0root_linux_service {
    const char *account;
    uint32_t filed_rights;
    uint32_t credential_rights;
    uint64_t clients;
    const char *ctty;
    unsigned argc;
    const char *argv[8];
} seed0root_linux_service_t;

#define SEED0ROOT_FILE_NAMESPACE (FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_WRITE | \
    FILED_RIGHT_EXEC | FILED_RIGHT_STAT | FILED_RIGHT_SETATTR | FILED_RIGHT_GETDENTS | \
    FILED_RIGHT_CREATE | FILED_RIGHT_REMOVE | FILED_RIGHT_RENAME)
#define SEED0ROOT_DESKTOP_CLIENTS (FILED_EXEC_SERVICE_NETD | FILED_EXEC_SERVICE_TERMD | \
    FILED_EXEC_SERVICE_DRMD | FILED_EXEC_SERVICE_INPUTD)
#define SEED0ROOT_READ_NAMESPACE (FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | \
    FILED_RIGHT_EXEC | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS)

static const seed0root_linux_service_t seed0root_linux_services[] = {
    { .account = "root", .filed_rights = SEED0ROOT_FILE_NAMESPACE,
      .clients = SEED0ROOT_DESKTOP_CLIENTS, .ctty = "/dev/hvc0",
#if defined(SEED0ROOT_CREDENTIAL_TEST) && SEED0ROOT_CREDENTIAL_TEST
      .argc = 2, .argv = {"/cmd/lpr_credentials.elf", "--nonzero"} },
#else
      .argc = 1, .argv = {"/sbin/init"} },
#endif
    /* The normal system bus, separate from the per-login session bus. The
     * supervisor assigns messagebus directly; no set-ID authority is issued.
     * Pathname bind/cleanup needs CREATE/REMOVE/SETATTR, not file-data WRITE. */
    { .account = "messagebus", .filed_rights = SEED0ROOT_READ_NAMESPACE |
        FILED_RIGHT_CREATE | FILED_RIGHT_REMOVE | FILED_RIGHT_SETATTR,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 8, .argv = {"/sbin/pacha-service-runtime", "/run/dbus", "--", "/usr/bin/dbus-daemon",
        "--config-file=/etc/dbus-1/pacha-system.conf", "--nofork", "--nopidfile", "--nosyslog"} },
    /* System services are launched by the same trusted manager, not through
     * a setuid D-Bus helper. UID zero does not supply any omitted client FD. */
    { .account = "polkitd", .filed_rights = SEED0ROOT_READ_NAMESPACE | FILED_RIGHT_CREATE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 6, .argv = {"/sbin/pacha-service-runtime", "--system-bus", "/run/polkit-1/rules.d", "/run/polkit-1/actions", "--",
        "/usr/lib/polkit-1/polkitd"} },
    { .account = "root", .filed_rights = SEED0ROOT_FILE_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD | FILED_EXEC_SERVICE_NETD, .ctty = "/dev/hvc0",
      .argc = 6, .argv = {"/sbin/pacha-service-runtime", "--bus-name", "org.freedesktop.PolicyKit1", "/var/lib/upower", "--",
        "/usr/libexec/upowerd"} },
#if defined(SEED0ROOT_REAL_SERVICE_TEST) && SEED0ROOT_REAL_SERVICE_TEST
    /* Real upstream daemon and clients; no new permanent system bus. No
     * identity-changing or writable filesystem grant is needed by this bus. */
    { .account = "messagebus", .filed_rights = SEED0ROOT_READ_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 5, .argv = {"/usr/bin/dbus-daemon",
        "--config-file=/etc/dbus-1/pacha-account-test.conf", "--fork", "--nopidfile", "--nosyslog"} },
    { .account = "messagebus", .filed_rights = SEED0ROOT_READ_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 2, .argv = {"/cmd/lpr_real_service.elf", "allowed"} },
    { .account = "user", .filed_rights = SEED0ROOT_READ_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 2, .argv = {"/cmd/lpr_real_service.elf", "denied-user"} },
    { .account = "root", .filed_rights = SEED0ROOT_READ_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 2, .argv = {"/cmd/lpr_real_service.elf", "denied-root"} },
#endif
#if defined(SEED0ROOT_ACCOUNT_LAUNCH_TEST) && SEED0ROOT_ACCOUNT_LAUNCH_TEST
    /* Exercise the same direct launch path without enabling a new production
     * daemon as a side effect of adding account support. */
    { .account = "messagebus", .filed_rights = SEED0ROOT_FILE_NAMESPACE,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .credential_rights = LPRS_CREDENTIAL_SETUID | LPRS_CREDENTIAL_SETGID,
      .argc = 2, .argv = {"/cmd/lpr_service_account.elf", "transitions"} },
    { .account = "user", .filed_rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_READ |
        FILED_RIGHT_EXEC | FILED_RIGHT_STAT | FILED_RIGHT_GETDENTS,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 2, .argv = {"/cmd/lpr_service_account.elf", "restricted"} },
    { .account = "root", .filed_rights = 0,
      .clients = FILED_EXEC_SERVICE_TERMD, .ctty = "/dev/hvc0",
      .argc = 2, .argv = {"/cmd/lpr_service_account.elf", "no-grants"} },
#endif
};
