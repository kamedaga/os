#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include "filed/live_bootstrap.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include <errno.h>
#include <elf.h>
#include <limits.h>
#include <string.h>

static pacha_fd_t bootstrap_fd(char **argv,filed_runtime_t *rt)
{
    if (!argv) return -EINVAL;
    char **p=argv;
    while (*p) ++p;
    ++p;
    while (*p) ++p;
    ++p;
    const uint64_t *aux=(const uint64_t *)(const void *)p;
    uint64_t phent=0;
    pacha_fd_t bootstrap=-ENOENT;
    for (unsigned i=0;i<64;++i) {
        if (!aux[2*i]) break;
        if (aux[2*i]==AT_PHDR) rt->executable_phdr=aux[2*i+1];
        if (aux[2*i]==AT_PHNUM) rt->executable_phnum=aux[2*i+1];
        if (aux[2*i]==AT_PHENT) phent=aux[2*i+1];
        if (aux[2*i]==PACHA_AT_BOOTSTRAP_FD)
            bootstrap=aux[2*i+1]>=16 && aux[2*i+1]<PACHA_THREAD_SELF_FD ?
                (int)aux[2*i+1] : -EINVAL;
    }
    if (phent!=sizeof(Elf64_Phdr) || !rt->executable_phdr ||
        !rt->executable_phnum || rt->executable_phnum>128) return -EINVAL;
    return bootstrap;
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
    pacha_fd_t fd=bootstrap_fd(argv,rt);
    if (fd<16) return fd;
    rt->bootstrap_fd=fd;
    struct pacha_fd_info info;
    if (pacha_fd_get_info(fd,&info) || info.kind!=PACHA_FD_KIND_VMO ||
        info.size<sizeof(storage_filed_bootstrap_t)) return -EINVAL;
    storage_filed_bootstrap_t bootstrap={0};
    long got=pacha_fd_read(fd,&bootstrap,sizeof(bootstrap));
    if (got!=(long)sizeof(bootstrap) ||
        bootstrap.magic!=STORAGE_FILED_BOOTSTRAP_MAGIC ||
        bootstrap.control_fd<16 || bootstrap.control_fd>=PACHA_THREAD_SELF_FD ||
        bootstrap.unix_path_fd<16 || bootstrap.unix_path_fd>=PACHA_THREAD_SELF_FD ||
        bootstrap.bootfs_fd<16 || bootstrap.bootfs_fd>=PACHA_THREAD_SELF_FD ||
        !bootstrap.bootfs_size) return -EINVAL;
    /* At 8 GiB this covers the measured 519 MiB boot distinct set while
     * retaining at most 768 MiB. Smaller machines retain at most RAM/8. */
    pacha_system_info_t memory={0};
    if (!pacha_syscall1(PACHA_RUNTIME_SYSCALL_SYSTEM_INFO,(uintptr_t)&memory)) {
        rt->snapshots.limit=memory.total_usable_memory_bytes/8;
        if (rt->snapshots.limit>UINT64_C(768)*1024*1024)
            rt->snapshots.limit=UINT64_C(768)*1024*1024;
    }
    rt->bootstrap=bootstrap;
    rt->client_endpoint_fd=(pacha_fd_t)bootstrap.control_fd;
    rt->unix_path_fd=(pacha_fd_t)bootstrap.unix_path_fd;
    return filed_storage_owner_create((pacha_fd_t)bootstrap.bootfs_fd,
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
