/* SPDX-License-Identifier: MIT */
#include "filed_client/client.h"
#include "pacha/ipc.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int filed_client_call_published(int endpoint, const kb2_fs_request_t *r,
    const void *tail, size_t tail_size, void *out, size_t capacity,
    kb2_fs_response_t *response, const int *sources, size_t source_count,
    int *reply_fds, size_t *reply_count, int *published)
{
    if (published) *published=0;
    if (endpoint < 16 || !r || !response || (tail_size && !tail) ||
        (capacity && !out) || (source_count && !sources) ||
        source_count > PACHA_IPC_MAX_TRANSFER_FDS - 2 ||
        tail_size > SIZE_MAX-KB2_FILESYSTEM_REQUEST_SIZE ||
        capacity > SIZE_MAX-KB2_FILESYSTEM_RESPONSE_SIZE) return -22;
    size_t request_size = KB2_FILESYSTEM_REQUEST_SIZE + tail_size;
    size_t response_size = KB2_FILESYSTEM_RESPONSE_SIZE + capacity;
    size_t size = request_size > response_size ? request_size : response_size;
    if (size > SIZE_MAX-4095) return -75;
    size = (size+4095)&~(size_t)4095;
    int vmo = pacha_vmo_create(size, PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, 0);
    if (vmo < 16) return vmo < 0 ? vmo : -5;
    uint8_t *page = pacha_mmap(vmo,size,PACHA_PROT_READ|PACHA_PROT_WRITE,
                                PACHA_MMAP_SHARED,0);
    if (!page) { (void)pacha_fd_close(vmo); return -12; }
    int status = kb2_fs_request_encode(page,KB2_FILESYSTEM_REQUEST_SIZE,
                                       request_size,r);
    if (!status && tail_size) memcpy(page+KB2_FILESYSTEM_REQUEST_SIZE,tail,tail_size);
    struct pacha_ipc_fd send[PACHA_IPC_MAX_TRANSFER_FDS] = {{
        .fd=(uint64_t)vmo,.rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
            PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE,
        .transfer_flags=PACHA_IPC_TRANSFER_CLOEXEC }};
    for (size_t i=0; !status && i<source_count; ++i) {
        struct pacha_fd_info info;
        status=pacha_fd_get_info(sources[i],&info);
        if (!status) send[i+1]=(struct pacha_ipc_fd){.fd=(uint64_t)sources[i],
            .rights=info.rights,.transfer_flags=PACHA_IPC_TRANSFER_CLOEXEC};
    }
    struct pacha_ipc_msg message = { .word0=r->opcode,.word1=request_size,
        .word2=response_size,.word3=r->sequence,.fds=send,.fd_count=source_count+1 };
    struct pacha_ipc_fd received[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg reply = { .fds=received,.fd_capacity=PACHA_IPC_MAX_TRANSFER_FDS };
    if (!status) {
        int reply_fd=pacha_ipc_call(endpoint,&message);
        if (reply_fd < 16) status=reply_fd < 0 ? reply_fd : -5;
        else {
            if (published) *published=1;
            status=pacha_ipc_recv_wait(reply_fd,&reply,PACHA_FD_WAIT_FOREVER);
            (void)pacha_fd_close(reply_fd);
        }
    }
    if (!status && (reply.word0 != r->opcode || reply.word2 > response_size ||
        reply.word2 < KB2_FILESYSTEM_RESPONSE_SIZE || reply.word3 != r->sequence ||
        kb2_fs_response_decode(page,(size_t)reply.word2,response) != 0 ||
        !kb2_fs_response_matches(response,r) ||
        (uint64_t)response->result != reply.word1)) status=-71;
    if (!status && response->data_length > capacity) status=-71;
    if (!status && response->data_length)
        memcpy(out,page+KB2_FILESYSTEM_RESPONSE_SIZE,(size_t)response->data_length);
    size_t accepted = !status && reply_count && reply_fds ? *reply_count : 0;
    if (!status && reply.fd_count>accepted) { status=-71; accepted=0; }
    if (accepted > reply.fd_count) accepted=reply.fd_count;
    for (size_t i=0; i<reply.fd_count; ++i) {
        if (i<accepted) reply_fds[i]=(int)received[i].fd;
        else (void)pacha_fd_close((int)received[i].fd);
    }
    if (reply_count) *reply_count=accepted;
    (void)pacha_munmap(page,size);
    (void)pacha_fd_close(vmo);
    return status;
}
int filed_client_call(int endpoint,const kb2_fs_request_t *r,
    const void *tail,size_t tail_size,void *out,size_t capacity,
    kb2_fs_response_t *response,const int *sources,size_t source_count,
    int *reply_fds,size_t *reply_count)
{
    return filed_client_call_published(endpoint,r,tail,tail_size,out,capacity,
        response,sources,source_count,reply_fds,reply_count,NULL);
}
int filedfs_call(int endpoint,kb2_fs_request_t *r,const char *path,
    const char *second,const void *data,size_t data_size,
    void *out,size_t capacity,kb2_fs_response_t *response,
    const int *sources,size_t source_count,int *received,size_t *received_count)
{
    static uint64_t sequence;
    if (!r || (data_size && !data)) return -22;
    if (!r->generation) r->generation=1;
    if (!r->sequence) {
        r->sequence=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
        if (!r->sequence) return -75;
    }
    size_t first_size=path ? strlen(path)+1 : 0;
    size_t second_size=second ? strlen(second)+1 : 0;
    if (first_size > SIZE_MAX-second_size ||
        data_size > SIZE_MAX-first_size-second_size) return -75;
    size_t tail_size=first_size+second_size+data_size;
    uint8_t *tail=tail_size ? malloc(tail_size) : NULL;
    if (tail_size && !tail) return -12;
    size_t used=0;
    r->path=(kb2_fs_span_t){0};
    r->second_path=(kb2_fs_span_t){0};
    r->data=(kb2_fs_span_t){0};
    if (first_size) {
        r->path=(kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE+used,first_size};
        memcpy(tail+used,path,first_size); used+=first_size;
    }
    if (second_size) {
        r->second_path=(kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE+used,second_size};
        memcpy(tail+used,second,second_size); used+=second_size;
    }
    if (data_size) {
        r->data=(kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE+used,data_size};
        memcpy(tail+used,data,data_size);
    }
    int status=filed_client_call(endpoint,r,tail,tail_size,out,capacity,response,
                                  sources,source_count,received,received_count);
    free(tail);
    return status;
}
