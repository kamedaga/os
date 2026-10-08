#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include "filed/live_bootstrap.h"
#include "pacha/ipc.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

static int bootstrap_fd(char **argv)
{
    if (!argv) return -EINVAL;
    char **p=argv;
    while (*p) ++p;
    ++p;
    while (*p) ++p;
    ++p;
    const uint64_t *aux=(const uint64_t *)(const void *)p;
    for (unsigned i=0;i<64;++i) {
        if (!aux[2*i]) break;
        if (aux[2*i]==PACHA_AT_BOOTSTRAP_FD)
            return aux[2*i+1]>=16 && aux[2*i+1]<PACHA_FD_TABLE_LIMIT ?
                (int)aux[2*i+1] : -EINVAL;
    }
    return -ENOENT;
}
void filed_runtime_init(filed_runtime_t *rt)
{
    if (!rt) return;
    memset(rt,0,sizeof(*rt));
    rt->bootstrap_fd=rt->client_endpoint_fd=rt->unix_path_fd=-1;
    rt->live_bootfs_fd=rt->live_ready_fd=-1;
    rt->netd_socket_endpoint_fd=rt->termd_tty_endpoint_fd=-1;
    rt->gpud_drm_endpoint_fd=rt->inputd_input_endpoint_fd=-1;
    rt->storage_generation=1;
    rt->client_sequence=1;
}
int filed_runtime_bootstrap(filed_runtime_t *rt,char **argv)
{
    if (!rt) return -EINVAL;
    int fd=bootstrap_fd(argv);
    if (fd<16) return fd;
    rt->bootstrap_fd=fd;
    struct pacha_fd_info info;
    if (pacha_fd_get_info(fd,&info) || info.kind!=PACHA_FD_KIND_VMO ||
        info.size<sizeof(storage_filed_bootstrap_t)) return -EINVAL;
    storage_filed_bootstrap_t bootstrap={0};
    long got=pacha_fd_read(fd,&bootstrap,sizeof(bootstrap));
    if (got!=(long)sizeof(bootstrap) ||
        bootstrap.magic!=STORAGE_FILED_BOOTSTRAP_MAGIC ||
        bootstrap.control_fd<16 || bootstrap.control_fd>=PACHA_FD_TABLE_LIMIT ||
        bootstrap.unix_path_fd<16 || bootstrap.unix_path_fd>=PACHA_FD_TABLE_LIMIT ||
        bootstrap.bootfs_fd<16 || bootstrap.bootfs_fd>=PACHA_FD_TABLE_LIMIT ||
        !bootstrap.bootfs_size) return -EINVAL;
    rt->bootstrap=bootstrap;
    rt->client_endpoint_fd=(int)bootstrap.control_fd;
    rt->unix_path_fd=(int)bootstrap.unix_path_fd;
    return filed_storage_owner_create((int)bootstrap.bootfs_fd,
        bootstrap.bootfs_size,&bootstrap,&rt->storage);
}
int filed_runtime_mount_root(filed_runtime_t *rt)
{
    if (!rt || !rt->storage) return -EINVAL;
    int status=filed_storage_owner_start(rt->storage);
    if (status) return status;
    kb2_fs_request_t hello={.opcode=KB2_FILESYSTEM_OP_HELLO};
    kb2_fs_response_t response;
    status=filed_owner_control(rt,&hello,NULL,KB2_FILESYSTEM_REQUEST_SIZE,&response);
    if (status) return status;
    /* Only the host owner registers this escrow/launch-manager identity.
     * Ordinary request bytes cannot select it or acquire Linux root creds. */
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE+KB2_FILESYSTEM_CREDENTIALS_SIZE]={0};
    kb2_fs_credentials_t credentials={.umask=0022,
        .cap_permitted=KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK,
        .cap_effective=KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK};
    status=kb2_fs_credentials_encode(bytes+KB2_FILESYSTEM_REQUEST_SIZE,
        KB2_FILESYSTEM_CREDENTIALS_SIZE,&credentials);
    kb2_fs_request_t register_owner={.opcode=KB2_FILESYSTEM_OP_CLIENT_REGISTER,
        .client=1,.credential_generation=1,
        .capabilities=KB2_FILESYSTEM_RIGHTS_VALID_MASK,
        .data={KB2_FILESYSTEM_REQUEST_SIZE,KB2_FILESYSTEM_CREDENTIALS_SIZE}};
    if (!status) status=filed_owner_control(rt,&register_owner,bytes,sizeof(bytes),&response);
    return status;
}
