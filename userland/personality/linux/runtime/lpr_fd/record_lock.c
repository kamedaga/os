/* SPDX-License-Identifier: MIT */
#include "../lpr_filed_internal.h"
extern volatile uint32_t lpr_posix_locks_used;

/* Shared by all filed POSIX record-lock operations. Keeping range conversion
 * and reply validation together prevents GETLK from becoming a pointer-valued
 * generic fcntl request or changing unspecified caller fields on no conflict. */
int64_t lpr_filed_record_lock(uint64_t fd,uint64_t cmd,uint64_t arg)
{
    /* Linux x86_64 flock is a userspace record, never a sandbox pointer.
     * Resolve relative ranges once before an interruptible blocking wait. */
    struct { int16_t type,whence;int32_t pad;int64_t start,length;int32_t pid,tail; } range;
    if (!lpr_user_range_plausible(arg,sizeof(range))) return -LPR_LINUX_EFAULT;
    lpr_memcpy(&range,(const void *)(uintptr_t)arg,sizeof(range));
    const __typeof__(range) original=range;
    int64_t base=0;
    if (range.whence==1) {
        base=lpr_linux_lseek(fd,0,1);
        if (base<0) return base;
    } else if (range.whence==2) {
        lpr_linux_stat_t stat;
        int64_t status=lpr_linux_fstat(fd,(uint64_t)(uintptr_t)&stat,0);
        if (status) return status;
        base=stat.st_size;
    } else if (range.whence) return -LPR_LINUX_EINVAL;
    if (__builtin_add_overflow(base,range.start,&range.start)) return -LPR_LINUX_EOVERFLOW;
    if (range.start<0) return -LPR_LINUX_EINVAL;
    if (cmd!=LPR_LINUX_F_GETLK) __atomic_store_n(&lpr_posix_locks_used,1,__ATOMIC_RELEASE);
    for (;;) {
        const int64_t binding_status=lpr_filed_fd_binding(fd);
        if (binding_status) return binding_status;
        const lpr_filed_backend_t *file=lpr_filed_backend(fd);
        if (!file) return -LPR_LINUX_EBADF;
        kb2_fs_request_t request={.opcode=KB2_FILESYSTEM_OP_FCNTL,
            .handle=file->handle,.flags=cmd==LPR_LINUX_F_GETLK ? cmd : LPR_LINUX_F_SETLK,.mode=(uint16_t)range.type,
            .offset=range.start,.length=(uint64_t)range.length};
        kb2_fs_response_t response;
        uint8_t payload[KB2_FILESYSTEM_RECORD_LOCK_SIZE];
        int64_t status=lpr_fs_call(&request,0,0,0,0,
            cmd==LPR_LINUX_F_GETLK ? payload : 0,
            cmd==LPR_LINUX_F_GETLK ? sizeof(payload) : 0,&response,-1,0);
        if (!status && cmd==LPR_LINUX_F_GETLK) {
            kb2_fs_record_lock_t record;
            if (kb2_fs_record_lock_decode(payload,response.data_length,&record))
                return -LPR_LINUX_EIO;
            /* No conflict changes only l_type; a conflict is absolute.
             * Never copy an unvalidated peer record into the caller. */
            range=original;
            range.type=(int16_t)record.type;
            if (record.type!=2) {
                range.whence=0; range.start=record.start;
                range.length=record.length; range.pid=record.pid;
            }
            lpr_memcpy((void *)(uintptr_t)arg,&range,sizeof(range));
        }
        if (cmd!=LPR_LINUX_F_SETLKW || status!=-LPR_LINUX_EAGAIN)
            return status;
        /* Keep a potentially long client wait out of storage workers.
         * Each attempt releases its request before sleeping, so process
         * death cannot leave a queued request acquiring an orphan lock. */
        const struct pachaos_timespec delay={.tv_sec=0,.tv_nsec=1000000};
        unsigned wait_not_ready=0;
        status=lpr_linux_nanosleep((uint64_t)(uintptr_t)&delay,0,&wait_not_ready);
        if (status) return status;
    }
}

