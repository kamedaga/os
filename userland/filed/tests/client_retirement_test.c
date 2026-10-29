/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include "no_audio_control.h"
#include <assert.h>
#include <stdio.h>

static filed_runtime_t runtime;
static struct filed_client *retiring;
static unsigned controls, releases, closes[100], old_completed;
static int remote_revoked, lost_control, lost_release, failed_close;

int filed_storage_owner_control_call(struct filed_storage_owner *owner,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *used)
{
    kb2_fs_request_t request;
    assert(owner==(void *)1234 && !kb2_fs_request_decode(input,size,size,&request));
    assert(request.opcode==KB2_FILESYSTEM_OP_CLIENT_UNREGISTER && request.client==9);
    assert(retiring->retiring && !retiring->revoked && !runtime.actor);
    ++controls;
    remote_revoked=1;
    if (lost_control) return -EIO; /* The operation happened, but its ACK was lost. */
    kb2_fs_response_t response={.opcode=request.opcode,.generation=request.generation,
        .sequence=request.sequence};
    assert(!kb2_fs_response_encode(output,capacity,&response));
    *used=KB2_FILESYSTEM_RESPONSE_SIZE;
    return 0;
}

int filed_storage_owner_client_release(struct filed_storage_owner *owner,uint64_t id)
{
    assert(owner==(void *)1234 && id==101+releases);
    assert(remote_revoked && retiring->revoked && retiring->retiring);
    assert(runtime.clients==retiring);
    /* A second queue shares the revoked identity, so it cannot admit a new
     * operation while this queue drains an already prepared write. */
    assert(runtime.sessions->client==retiring);
    ++old_completed;
    ++releases;
    return lost_release==(int)releases ? -EIO : 0;
}

int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd==60 || (fd>=90 && fd<=93));
    assert(retiring->revoked);
    if (fd==60) assert(!runtime.sessions && releases==2);
    else assert(releases>=(unsigned)(fd<=91 ? 1 : 2));
    ++closes[fd];
    return failed_close==fd ? -EBUSY : 0;
}

static void setup(void)
{
    controls=releases=old_completed=0;
    remote_revoked=lost_control=lost_release=failed_close=0;
    memset(closes,0,sizeof(closes));
    retiring=calloc(1,sizeof(*retiring));
    struct filed_client *other=calloc(1,sizeof(*other));
    assert(retiring && other);
    *other=(struct filed_client){.id=10,.fd=61};
    *retiring=(struct filed_client){.next=other,.id=9,.credential_generation=7,.fd=60};
    runtime=(filed_runtime_t){.clients=retiring,.actor=retiring,
        .storage=(void *)1234,.storage_generation=77};
    struct filed_session **tail=&runtime.sessions;
    for (unsigned i=0;i<2;++i) {
        struct filed_session *s=calloc(1,sizeof(*s));
        assert(s);
        *s=(struct filed_session){.client=retiring,.config={.generation=77,.channel_id=101+i},
            .vmo_fd=90+2*(int)i,.doorbell_fd=91+2*(int)i,.attached=1};
        *tail=s;tail=&s->next;
    }
}

static void dispose_stopped_fixture(void)
{
    /* No real mappings/capabilities exist here. This disposal represents the
     * stopped sandbox only, not permission to free uncertain live inventory. */
    while (runtime.sessions) {
        struct filed_session *s=runtime.sessions;
        runtime.sessions=s->next;free(s);
    }
    while (runtime.clients) {
        struct filed_client *c=runtime.clients;
        runtime.clients=c->next;free(c);
    }
}

int main(void)
{
    setup();
    assert(!filed_client_release(&runtime,retiring));
    assert(runtime.clients->id==10 && !runtime.clients->retiring && !runtime.sessions);
    assert(!runtime.actor && runtime.storage_generation==77 && controls==1 && releases==2);
    assert(old_completed==2 && closes[90]==1 && closes[91]==1 &&
        closes[92]==1 && closes[93]==1 && closes[60]==1);
    dispose_stopped_fixture();
    for (unsigned failure=0;failure<6;++failure) {
        setup();
        if (!failure) lost_control=1;
        else if (failure<=2) lost_release=(int)failure;
        else failed_close=failure==3 ? 90 : failure==4 ? 91 : 60;
        assert(filed_client_release(&runtime,retiring)==(failure<3 ? -EIO : -EBUSY));
        assert(runtime.clients==retiring && retiring->retiring && retiring->fd==60);
        assert(!runtime.actor && !runtime.storage_generation && controls==1);
        assert(retiring->revoked==!!failure);
        if (!failure) assert(!releases && !closes[90] && !closes[60]);
        else if (failure<=2) {
            assert(runtime.sessions && !runtime.sessions->remote_released);
            assert(runtime.sessions->config.channel_id==100+failure);
            assert(!closes[runtime.sessions->vmo_fd] && !closes[60]);
        } else if (failure<=4) {
            assert(runtime.sessions && runtime.sessions->remote_released);
            assert(runtime.sessions->vmo_fd==(failure==3 ? 90 : -1));
            assert(runtime.sessions->doorbell_fd==91 && !closes[60]);
        } else assert(!runtime.sessions && closes[60]==1);
        unsigned before=releases;
        assert(filed_client_release(&runtime,retiring)==-EIO);
        assert(releases==before && controls==1); /* Never repeat an ambiguous operation. */
        dispose_stopped_fixture();
    }
    puts("filed client retirement: revoke-all-before-drain / matching ACK / lost ACK / partial CLOSE inventory PASS");
    return 0;
}
