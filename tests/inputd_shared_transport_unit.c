/* Exercise production producer, staging, ioctl, grab and detach code. */
#define main history_unit_main
#include "inputd_ring_overflow_unit.c"
#undef main
#include <assert.h>
#include <stdio.h>
static struct inputd_event_ring shared[2];
int pacha_munmap(void *p,uint64_t n){(void)p;assert(n==INPUTD_RING_BYTES);return 0;}
int pacha_fd_close(pacha_fd_t fd){(void)fd;return 0;}
static void frame(int value){kb_input_event_t x={.device_id=1,.type=2,.code=0,.value=value}, y=x, syn={.device_id=1,.type=0,.code=0};y.code=1;y.value=-value;stage_event(&x,1000000000);stage_event(&y,1000000000);stage_event(&syn,1000000000);}
int main(void){reset_inputd(1);setup_device(0,1);ioctl_snapshot.id=1;setup_handle(1,next_sequence);handles[0].notify_fd=16;assert(!inputd_input_attach_ring(1,&shared[0]));handles[1]=handles[0];handles[1].id=2;handles[1].shared=0;assert(!inputd_input_attach_ring(2,&shared[1]));test_registry[0].handle_mask=3;
inputd_input_event_t out[2048];frame(9);assert(inputd_ring_read(&shared[0],out,2048)==3&&out[0].value==9);assert(inputd_ring_read(&shared[1],out,2048)==3&&out[1].value==-9);assert(inputd_ring_read(&shared[0],out,2048)==-11);frame(10);frame(11);assert(inputd_ring_read(&shared[0],out,2048)==6&&out[3].value==11);
for(int i=0;i<800;i++)frame(i);int n=inputd_ring_read(&shared[0],out,2048);assert(n>0&&out[0].type==0&&out[0].code==3&&out[n-1].type==0&&out[n-1].code==0);
/* State snapshots remove buffered events of the requested type. */
kb_input_event_t key={.device_id=1,.type=1,.code=30,.value=1},syn={.device_id=1,.type=0,.code=0};stage_event(&key,1000000000);stage_event(&syn,1000000000);shared_flush_type(&handles[0],1);n=inputd_ring_read(&shared[0],out,2048);assert(n==1&&out[0].type==0&&out[0].code==0);
frame(12);inputd_ioctl_request_t request={.handle=1,.request=INPUTD_EVIOCSCLOCKID,.data_size=4};int clock=INPUTD_CLOCK_REALTIME;memcpy(request.data,&clock,4);assert(!inputd_input_ioctl(&request));assert(inputd_ring_read(&shared[0],out,2048)==-11);frame(13);assert(inputd_ring_read(&shared[0],out,2048)==4&&out[0].code==3&&out[1].value==13);
/* A grab affects future frames, not another open's old unread queue. */
while(inputd_ring_read(&shared[1],out,2048)>0){}test_registry[0].grabbed_handle=1;frame(14);assert(inputd_ring_read(&shared[1],out,2048)==-11);assert(inputd_ring_read(&shared[0],out,2048)==3);test_registry[0].grabbed_handle=0;frame(15);assert(inputd_ring_read(&shared[1],out,2048)==3&&out[0].value==15);
request=(inputd_ioctl_request_t){.handle=1,.request=INPUTD_EVIOCREVOKE};assert(!inputd_input_ioctl(&request));assert(inputd_ring_read(&shared[0],out,2048)==-19);assert(!inputd_input_dup(1,&request.handle));assert(!inputd_input_close(1));assert(handles[0].active);assert(!inputd_input_close(1));assert(!handles[0].active);test_registry[0].source_id=1;test_registry[0].seen=0;assert(!inputd_input_source_end(active_island,1,1,0,0));assert(inputd_ring_read(&shared[1],out,2048)==-19);puts("PASS inputd shared transport: producer batches, per-open fanout, overflow, state flush, clock flush, grab/revoke and dup close");return 0;}
