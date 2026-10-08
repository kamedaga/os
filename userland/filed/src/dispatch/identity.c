#include "filed/identity.h"
#include "filed/runtime.h"
#include "filed/storage_owner.h"
#include "filed/ipc_protocol.h"
#include "pacha/ipc.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int filed_owner_control(filed_runtime_t *rt, kb2_fs_request_t *r,
                         const uint8_t *original, size_t size,
                         kb2_fs_response_t *response)
{
    if (!rt || !r || !response || size < KB2_FILESYSTEM_REQUEST_SIZE)
        return -22;
    uint8_t *copy = malloc(size);
    if (!copy) return -12;
    if (original) memcpy(copy,original,size);
    else memset(copy,0,size);
    r->generation=rt->storage_generation;
    if (rt->request_sequence==UINT64_MAX) { free(copy); return -75; }
    r->sequence=++rt->request_sequence;
    int status=kb2_fs_request_encode(copy,KB2_FILESYSTEM_REQUEST_SIZE,size,r);
    uint8_t reply[KB2_FILESYSTEM_RESPONSE_SIZE];
    size_t used=0;
    if (!status) status=filed_storage_owner_control_call(rt->storage,
        copy,size,reply,sizeof(reply),&used);
    if (!status && (kb2_fs_response_decode(reply,used,response) ||
                    !kb2_fs_response_matches(response,r))) status=-71;
    free(copy);
    /* A registry update can have executed before a lost/malformed reply.
     * Local cached authority must not keep serving an ambiguous generation.
     * An encoded Linux operation error, unlike a transport error, is certain. */
    if (status) { rt->storage_generation=0; return status; }
    return response->result < 0 ? (int)response->result : 0;
}
static int credentials_valid(const kb2_fs_request_t *r,const uint8_t *bytes,
                              size_t size)
{
    kb2_fs_credentials_t credentials;
    if (!r || !bytes || !r->credential_generation || r->offset <= 0 ||
        r->offset > INT32_MAX ||
        r->capabilities & ~(uint64_t)KB2_FILESYSTEM_RIGHTS_VALID_MASK ||
        r->data.length != KB2_FILESYSTEM_CREDENTIALS_SIZE ||
        r->data.offset > size || r->data.length > size-r->data.offset ||
        r->groups.offset > size || r->groups.length > size-r->groups.offset ||
        kb2_fs_credentials_decode(bytes+r->data.offset,(size_t)r->data.length,
                                   &credentials)) return -22;
    return 0;
}
int filed_client_create(filed_runtime_t *rt,const kb2_fs_request_t *input,
    const uint8_t *bytes,size_t size,int *client_fd,uint64_t *id)
{
    if (!rt || rt->actor || !client_fd || !id) return -1;
    int status=credentials_valid(input,bytes,size);
    if (status) return status;
    if (rt->client_sequence==UINT64_MAX) return -75;
    struct filed_client *client=calloc(1,sizeof(*client));
    if (!client) return -12;
    struct pacha_ipc_channel_pair pair={-1,-1};
    uint64_t rights=PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|
        PACHA_FD_RIGHT_CALL|PACHA_FD_RIGHT_RECV|PACHA_FD_RIGHT_WAIT|
        PACHA_FD_RIGHT_POLL|PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_TRANSFER|
        PACHA_FD_RIGHT_SET_FLAGS;
    status=pacha_ipc_channel_create(&pair,rights,0);
    if (status) { free(client); return status; }
    uint64_t identity=++rt->client_sequence;
    kb2_fs_request_t control=*input;
    control.client=identity;
    kb2_fs_response_t response;
    status=filed_owner_control(rt,&control,bytes,size,&response);
    if (status) {
        (void)pacha_fd_close(pair.a); (void)pacha_fd_close(pair.b);
        free(client); return status;
    }
    *client=(struct filed_client){.next=rt->clients,.id=identity,
        .credential_generation=input->credential_generation,.fd=pair.a,
        .subject_generation=input->second_handle ? input->second_handle :
            input->credential_generation,
        .rights=(uint32_t)input->capabilities,.pid=(int32_t)input->offset};
    rt->clients=client;
    *client_fd=pair.b;
    *id=identity;
    return 0;
}
int filed_client_credentials(filed_runtime_t *rt,const kb2_fs_request_t *input,
    const uint8_t *bytes,size_t size)
{
    if (!rt || rt->actor) return -1;
    int status=credentials_valid(input,bytes,size);
    if (status) return status;
    struct filed_client *client=rt->clients;
    while (client && client->id!=input->client) client=client->next;
    if (!client) return -3;
    if (client->retiring) return -3;
    if (input->offset!=client->pid ||
        input->credential_generation<=client->credential_generation) return -1;
    kb2_fs_request_t control=*input;
    kb2_fs_response_t response;
    status=filed_owner_control(rt,&control,bytes,size,&response);
    if (!status) {
        client->credential_generation=input->credential_generation;
        client->rights=(uint32_t)input->capabilities;
    }
    return status;
}
int filed_client_release(filed_runtime_t *rt,struct filed_client *client)
{
    if (!rt || !client) return -22;
    client->retiring=1;
    if (rt->actor==client) rt->actor=NULL;
    if (!rt->storage_generation) return -5;
    /* Revoke the shared identity before draining any queue: otherwise a
     * second queue can keep admitting work while the first one's ACK waits.
     * Already prepared jobs retain their client/authority and may finish. */
    if (!client->revoked) {
        kb2_fs_request_t r={.opcode=KB2_FILESYSTEM_OP_CLIENT_UNREGISTER,.client=client->id,
            .credential_generation=client->credential_generation};
        kb2_fs_response_t response;
        int status=filed_owner_control(rt,&r,NULL,KB2_FILESYSTEM_REQUEST_SIZE,&response);
        if (status) { rt->storage_generation=0; return status; }
        client->revoked=1;
    }
    int status=filed_sessions_release_client(rt,client);
    if (!status) status=pacha_fd_close(client->fd);
    /* Sessions retain this pointer. Lost ACK or failed CLOSE keeps both the
     * client and its exact remaining inventory; it is not successful release. */
    if (status) { rt->storage_generation=0; return status; }
    client->fd=-1;
    struct filed_client **cursor=&rt->clients;
    while (*cursor && *cursor!=client) cursor=&(*cursor)->next;
    if (*cursor) *cursor=client->next;
    free(client);
    return 0;
}
