/* Reuse the native device fixture, then link the real owner RPC and real
 * sandbox queue. Only syscall transport and the exporter are modeled. */
#define ph_ipc_send fixture_ipc_send
#define main gpu_query_fixture_main
#include "gpud_gpu_query_unit.c"
#undef main
#undef ph_ipc_send
int ph_ipc_send(struct ph_ipc *,struct ph_ipc_packet *);
#include "../userland/gpud/gpu_rpc.c"
static struct ph_lifecycle_service rpc_service;
static unsigned corrupt_reply;
static int event_mode;
static unsigned event_order, immediate_reposts;
static struct ph_ipc_packet wake_fifo[64], command_pending;
static unsigned wake_read, wake_write;
static uint64_t event_session;
static void event_backend_step(struct ph_ipc *ipc) {
 assert(!rpc_service.dispatch(rpc_service.context,&calls));
 assert(!rpc_service.complete(rpc_service.context,ipc));
 assert(!rpc_service.release(rpc_service.context));
}
static void event_backend_ready(struct ph_ipc *ipc) {
 struct ph_gpu_queue *q=rpc_service.context;
 atomic_store(&q->event_cookies[0],1);
 atomic_fetch_or(&q->event_pending,1);
 assert(!rpc_service.next(q));
 assert(q->event_active);
 event_backend_step(ipc);
}
int ph_ipc_send(struct ph_ipc *ipc,struct ph_ipc_packet *packet) {
 if(!event_mode)return fixture_ipc_send(ipc,packet);
 assert(packet->operation==PH_GPU_QUEUE_NOTIFY && packet->generation==9);
 if(!(packet->value&1)) {
  assert(wake_write<64);wake_fifo[wake_write++]=*packet;
 } else if(packet->value==1) {
  int result=rpc_service.prepare(rpc_service.context,packet);
  if(result==0) {
   assert(immediate_reposts);--immediate_reposts;
   event_backend_step(ipc);
   if(immediate_reposts) {
    struct ph_gpu_queue *q=rpc_service.context;
    atomic_store(&q->event_cookies[0],1);atomic_fetch_or(&q->event_pending,1);
    assert(rpc_service.next(q)==PH_LIFECYCLE_SERVICE_IDLE);
   }
  } else assert(result==PH_LIFECYCLE_SERVICE_IDLE);
 } else {assert(!command_pending.operation);command_pending=*packet;}
 return 0;
}
static int event_receive(struct ph_ipc *ipc,struct ph_ipc_packet *packet) {
 if(wake_read==wake_write && command_pending.operation) {
  if(event_order==0)event_backend_ready(ipc);
  struct ph_ipc_packet request=command_pending;command_pending=(struct ph_ipc_packet){0};
  assert(!rpc_service.prepare(rpc_service.context,&request));
  event_backend_step(ipc);
  if(event_order==1)event_backend_ready(ipc);
 }
 if(wake_read==wake_write)return -EAGAIN;
 *packet=wake_fifo[wake_read++];return 0;
}
static int rpc_event_read(struct kobox_linux_drm_file *file,void *out,size_t cap,size_t *bytes) {
 assert(file && cap>=64);unsigned char *p=out;
 /* Two complete DRM vblank/flip records in one queue completion. */
 memset(p,0,64);
 for(unsigned i=0;i<2;i++) {ph_gpu_event_store_u32(p+i*32, i+1);ph_gpu_event_store_u32(p+i*32+4,32);}
 *bytes=64;return 0;
}
int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
 assert(!packet->fd_count);*packet=(struct ph_ipc_packet){0};return 0;
}
int ph_ipc_receive(struct ph_ipc *ipc,uint64_t gen,struct ph_ipc_packet *packet) {
 assert(ipc->generation==gen);if(event_mode)return event_receive(ipc,packet);return -EAGAIN;
}
int ph_ipc_receive_wait(struct ph_ipc *ipc,uint64_t gen,struct ph_ipc_packet *packet,uint64_t timeout) {
 if(event_mode) {assert(ipc->generation==gen && timeout);return event_receive(ipc,packet);}
 assert(ipc->generation==gen && timeout && sent.operation==PH_GPU_QUEUE_NOTIFY && sent.value==3);
 struct ph_ipc_packet request=sent;
 assert(!rpc_service.prepare(rpc_service.context,&request));
 struct ph_gpu_queue *queue=rpc_service.context;
 if(queue->sync_file.opcode) {
  /* Neither a hostile shared producer nor later reuse of raw request
   * storage can change the already admitted object/flags/session. */
  memset(queue_vmo+GPUD_GPU_CONTROL_REQUEST_OFFSET,0xcc,128);
  memset(queue->service.query.request,0xa5,128);
 }
 assert(!rpc_service.dispatch(rpc_service.context,&calls));
 assert(!rpc_service.complete(rpc_service.context,ipc));
 assert(!rpc_service.release(rpc_service.context));
 assert(rpc_service.next(rpc_service.context)==PH_LIFECYCLE_SERVICE_IDLE);
 if(corrupt_reply) {
  unsigned char *reply=queue_vmo+GPUD_GPU_CONTROL_REPLY_OFFSET;
  unsigned char *body=reply+KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
  const uint32_t opcode=ph_gpu_event_load_u32(reply+KB2_PROTOCOL_MESSAGE_ENVELOPE_OPCODE_OFFSET);
  const size_t body_size=opcode==KB2_GPU_OPCODE_DMA_BUF_CPU_SYNC ?
   KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_SIZE:kb2_gpu_sync_file_response_size(opcode);
  kb2_protocol_message_envelope_t e;
  assert(!kb2_protocol_message_envelope_decode(reply,
   KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+body_size,&e));
  if(corrupt_reply==1)++e.generation;
  if(corrupt_reply==2)++e.correlation_id;
  if(corrupt_reply==3)e.opcode=KB2_GPU_OPCODE_SESSION_CLOSE;
  assert(!kb2_protocol_message_envelope_encode(reply,
   KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+body_size,&e));
  if(corrupt_reply==4)ph_gpu_event_store_u64(body,8193);
  if(corrupt_reply==5)ph_gpu_event_store_u64(body+8,1);
  if(corrupt_reply==6)ph_gpu_event_store_u64(body+body_size-8,2);
  if(corrupt_reply==7)ph_gpu_event_store_u64(body+body_size-8,(uint64_t)(int64_t)-4096);
  if(corrupt_reply==8)ph_gpu_event_store_u64(body+16,999);
  if(corrupt_reply==9)ph_gpu_event_store_u64(body+24,0);
  if(corrupt_reply==10)ph_gpu_event_store_u64(body+16,2);
 }
 *packet=sent;return 0;
}
static void rpc_roundtrip(unsigned corruption) {
 struct ph_gpu_queue queue={0}; struct gpud_gpu_sessions sessions={0};
 struct gpud_gpu_channel frontend={0};struct ph_ipc ipc={.generation=9};
 memset(queue_vmo,0,sizeof(queue_vmo));active_vmo=queue_vmo;active_size=sizeof(queue_vmo);
 mapped=unmapped=aux_unmapped=sends=0;
 assert(!ph_gpu_channel_bind(&frontend,queue_vmo,9,27,KB2_VQ_DRIVER,&kb2_vq_x86_64_atomics));
 assert(!ph_gpu_sessions_init(&sessions,9,2));
 assert(!ph_gpu_queue_init_profile(&queue,&sessions,7,27,KB2_GPU_PROFILE_I915,
  &kb2_vq_x86_64_atomics,&rpc_service));
 struct ph_ipc_packet bind={.operation=PH_GPU_QUEUE_BIND,.generation=9,.value=27,.fd_count=1,
  .fds={{.fd=16,.rights=PH_GPU_QUERY_RIGHTS,.flags=PACHA_FD_FLAG_CLOEXEC}}};
 assert(rpc_service.prepare(rpc_service.context,&bind)==PH_LIFECYCLE_SERVICE_IDLE);
 struct gpud_gpu_rpc rpc={.ipc=&ipc,.channel=&frontend,.mapping=queue_vmo};
 corrupt_reply=corruption;cpu_sync_error=0;
 if(corruption) {
  assert(gpud_gpu_rpc_prime_cpu_sync(&rpc,1,4097,3)==-EPROTO && rpc.error==-EPROTO);
  const unsigned first_line=rpc.failure_line;
  assert(first_line);
  assert(!rpc_failed(&rpc,0,first_line+1));
  assert(rpc_failed(&rpc,-EIO,first_line+2)==-EIO);
  assert(rpc.error==-EPROTO && rpc.failure_line==first_line);
  assert(gpud_gpu_rpc_prime_cpu_sync(&rpc,2,4097,3)==-EPROTO);
  assert(rpc.error==-EPROTO && rpc.failure_line==first_line);
 } else {
  assert(gpud_gpu_rpc_prime_cpu_sync(&rpc,1,0,3)==-EINVAL && !rpc.error);
  assert(gpud_gpu_rpc_prime_cpu_sync(&rpc,1,4097,UINT64_C(1)<<32|1)==-EINVAL && !rpc.error);
  const uint64_t flags[]={1,2,3,5,6,7};
  for(unsigned i=0;i<6;++i) {
   assert(!gpud_gpu_rpc_prime_cpu_sync(&rpc,i+1,4097,flags[i]));
   assert(cpu_sync_token==4097 && cpu_sync_flags==flags[i] && !rpc.error);
  }
  const int errors[]={-EINTR,-EAGAIN,-EIO,-EACCES,-ENOENT};
  for(unsigned i=0;i<5;++i) {
   cpu_sync_error=errors[i];
   assert(gpud_gpu_rpc_prime_cpu_sync(&rpc,i+7,4097,3)==errors[i] && !rpc.error);
  }
  cpu_sync_error=0;
  assert(!gpud_gpu_rpc_prime_cpu_sync(&rpc,12,4097,7) && !rpc.error);
 }
 assert(!sessions.occupied && !rpc_service.stop(rpc_service.context));
}
static void sync_file_rpc_roundtrip(unsigned op,unsigned corruption) {
 struct ph_gpu_queue queue={0};struct gpud_gpu_sessions sessions={0};
 struct gpud_gpu_channel frontend={0};struct ph_ipc ipc={.generation=9};
 memset(queue_vmo,0,sizeof(queue_vmo));active_vmo=queue_vmo;active_size=sizeof(queue_vmo);
 mapped=unmapped=aux_unmapped=sends=0;
 assert(!ph_gpu_channel_bind(&frontend,queue_vmo,9,27,KB2_VQ_DRIVER,&kb2_vq_x86_64_atomics));
 assert(!ph_gpu_sessions_init(&sessions,9,2));
 assert(!ph_gpu_queue_init_profile(&queue,&sessions,7,27,KB2_GPU_PROFILE_I915,
  &kb2_vq_x86_64_atomics,&rpc_service));
 struct ph_ipc_packet bind={.operation=PH_GPU_QUEUE_BIND,.generation=9,.value=27,.fd_count=1,
  .fds={{.fd=16,.rights=PH_GPU_QUERY_RIGHTS,.flags=PACHA_FD_FLAG_CLOEXEC}}};
 assert(rpc_service.prepare(rpc_service.context,&bind)==PH_LIFECYCLE_SERVICE_IDLE);
 struct gpud_gpu_rpc rpc={.ipc=&ipc,.channel=&frontend,.mapping=queue_vmo};
 corrupt_reply=corruption;sync_file_status=0;
 uint64_t exported=0xfeed;
 if(corruption) {
  int result=op==0 ? gpud_gpu_rpc_prime_export_sync_file(&rpc,1,4097,3,103,&exported) :
   op==1 ? gpud_gpu_rpc_prime_import_sync_file(&rpc,1,4097,3,917) :
    gpud_gpu_rpc_release_sync_file(&rpc,1,917);
  assert(result==-EPROTO && rpc.error==-EPROTO && exported==0xfeed);
 } else {
  assert(gpud_gpu_rpc_prime_export_sync_file(&rpc,1,4097,3,0,&exported)==-EINVAL);
  assert(gpud_gpu_rpc_prime_import_sync_file(&rpc,1,4097,3,0)==-EINVAL);
  assert(gpud_gpu_rpc_release_sync_file(&rpc,1,0)==-EINVAL);
  assert(!rpc.error);
  const int errors[]={0,-EBUSY,-ENOMEM,-ENOENT,-EINTR,-EAGAIN,0};
  for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
   sync_file_status=errors[i];exported=0xfeed;
   int result=op==0 ? gpud_gpu_rpc_prime_export_sync_file(&rpc,i+1,4097,3,103,&exported) :
    op==1 ? gpud_gpu_rpc_prime_import_sync_file(&rpc,i+1,4097,3,917) :
     gpud_gpu_rpc_release_sync_file(&rpc,i+1,917);
   assert(result==errors[i] && !rpc.error && !rpc.failure_line);
   assert(exported==(!op && !result ? 917u:0xfeedu));
   if(!op) assert(sync_export_session==103 && sync_export_correlation==i+1);
   if(op==1) assert(sync_import_token==917);
   assert(!queue.sync_file.opcode && !queue.sync_file_exported);
  }
  if(op<2) {
   const unsigned before=calls;
   int result=op ? gpud_gpu_rpc_prime_import_sync_file(&rpc,8,4097,UINT64_C(1)<<32|3,917):
    gpud_gpu_rpc_prime_export_sync_file(&rpc,8,4097,UINT64_C(1)<<32|3,103,&exported);
   assert(result==-EINVAL && !rpc.error && calls==before);
  }
 }
 sync_file_status=0;
 assert(!sessions.occupied && !rpc_service.stop(rpc_service.context));
}
static void sync_file_codec_admission(void) {
 const uint32_t ops[]={KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE,
  KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE,KB2_GPU_OPCODE_SYNC_FILE_RELEASE};
 for(unsigned i=0;i<3;++i) {
  kb2_gpu_sync_file_request_t r={.opcode=ops[i],.token=917,
   .flags=i==2?0:UINT64_C(0x100000003),.session_id=i?0:103,.sync_file_token=i==1?17:0};
  unsigned char bytes[64]={0};size_t size=kb2_gpu_sync_file_request_size(r.opcode);
  assert(!kb2_gpu_sync_file_request_encode(bytes,size,&r));
  kb2_gpu_sync_file_request_t out={0};
  assert(!kb2_gpu_sync_file_request_decode(bytes,size,r.opcode,&out));
  assert(out.token==r.token && out.flags==r.flags && out.session_id==r.session_id &&
   out.sync_file_token==r.sync_file_token && out.opcode==r.opcode);
  out.token=0xfeed;
  assert(kb2_gpu_sync_file_request_decode(bytes,size-1,r.opcode,&out) && out.token==0xfeed);
  assert(kb2_gpu_sync_file_request_decode(bytes,size+1,r.opcode,&out) && out.token==0xfeed);
  if(!i) {
   kb2_gpu_sync_file_store(bytes+24,1);
   assert(kb2_gpu_sync_file_request_decode(bytes,size,r.opcode,&out) && out.token==0xfeed);
  }
  r.token=0;
  assert(kb2_gpu_sync_file_request_encode(bytes,size,&r));
 }
 puts("Sync_file control codec: exact bounds, reserved/zero-token refusal, full-width snapshot and untouched outputs on failure PASS");
}

static void event_rpc_ordering(unsigned order) {
 struct ph_gpu_queue queue={0};struct gpud_gpu_sessions sessions={0};
 struct gpud_gpu_channel frontend={0};struct ph_ipc ipc={.generation=9};
 memset(queue_vmo,0,sizeof(queue_vmo));active_vmo=queue_vmo;active_size=sizeof(queue_vmo);
 mock_send_error=0;mock_cookie=1;corrupt_reply=0;
 assert(!ph_gpu_channel_bind(&frontend,queue_vmo,9,27,KB2_VQ_DRIVER,&kb2_vq_x86_64_atomics));
 assert(!ph_gpu_sessions_init(&sessions,9,2));
 assert(!ph_gpu_queue_init_profile(&queue,&sessions,7,27,KB2_GPU_PROFILE_I915,&kb2_vq_x86_64_atomics,&rpc_service));
 struct ph_ipc_packet bind={.operation=PH_GPU_QUEUE_BIND,.generation=9,.value=27,.fd_count=1,
  .fds={{.fd=16,.rights=PH_GPU_QUERY_RIGHTS,.flags=PACHA_FD_FLAG_CLOEXEC}}};
 assert(rpc_service.prepare(rpc_service.context,&bind)==PH_LIFECYCLE_SERVICE_IDLE);
 assert(!ph_gpu_session_open_begin(&sessions,9,7,KB2_GPU_NODE_PRIMARY,&event_session));
 assert(!ph_gpu_session_open_finish(&sessions,9,7,event_session,1,KB2_GPU_STATUS_OK));
 queue.service.query.api.read_events=rpc_event_read;
 struct gpud_gpu_rpc rpc={.ipc=&ipc,.channel=&frontend,.mapping=queue_vmo};
 event_order=order;wake_read=wake_write=immediate_reposts=0;command_pending=(struct ph_ipc_packet){0};event_mode=1;
 assert(!gpud_gpu_rpc_start_events(&rpc));
 struct gpud_drm_binding binding={.generation=9,.frontend_handle=8,.session_id=event_session,.profile_kind=KB2_GPU_PROFILE_I915};
 gpud_drm_ioctl_request_t request={.handle=8,.request=0x641e};
 struct gpud_drm_translation encoded;
 assert(!gpud_drm_ioctl_encode(&encoded,&binding,&request,1));
 assert(encoded.queue_class==KB2_GPU_QUEUE_DISPLAY);
 unsigned char input[2048],reply[GPUD_GPU_CHANNEL_PAGE];
 kb2_protocol_message_envelope_t envelope={.protocol_id=KB2_GPU_PROTOCOL_ID,.opcode=KB2_GPU_OPCODE_COMMAND,
  .generation=9,.correlation_id=1,.payload_length=encoded.command_size};
 assert(!kb2_protocol_message_envelope_encode(input,sizeof(input),&envelope));
 memcpy(input+KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,encoded.command,encoded.command_size);
 size_t reply_size=0;
 assert(!gpud_gpu_rpc_call(&rpc,GPUD_GPU_QUEUE_DISPLAY,input,
  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+encoded.command_size,reply,sizeof(reply),&reply_size));
 /* Follow a display exchange with control then the actual failing command.
  * FIFO carries only wakes emitted by real native queue completion. */
 event_order=2;
 assert(!gpud_gpu_rpc_prime_cpu_sync(&rpc,2,4097,3));
 gpud_drm_syncobj_wait_t wait={.count_handles=1};
 request=(gpud_drm_ioctl_request_t){.handle=8,.request=0xc02864c3,
  .arg_size=sizeof(wait),.data_size=sizeof(wait),.aux_size=4};
 memcpy(request.data,&wait,sizeof(wait));
 assert(!gpud_drm_ioctl_encode(&encoded,&binding,&request,PH_GPU_QUERY_OUTPUT_REGION));
 assert(encoded.command_set_id==1 && encoded.command_id==19 && encoded.queue_class==KB2_GPU_QUEUE_EXECUTION);
 envelope.correlation_id=3;envelope.payload_length=encoded.command_size;
 assert(!kb2_protocol_message_envelope_encode(input,sizeof(input),&envelope));
 memcpy(input+KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,encoded.command,encoded.command_size);
 const uint32_t handle=3;memcpy(queue_vmo+GPUD_GPU_AUX_OFFSET,&handle,sizeof(handle));
 syncobj_wait_status=0;
 assert(!gpud_gpu_rpc_call(&rpc,GPUD_GPU_QUEUE_EXECUTION,input,
  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+encoded.command_size,reply,sizeof(reply),&reply_size));
 assert(!rpc.error && !rpc.failure_line);
 assert(wake_read==wake_write);
 /* Completion during repost: execute backend inside the available IPC send. */
 immediate_reposts=3;
 atomic_store(&queue.event_cookies[0],1);atomic_fetch_or(&queue.event_pending,1);
 assert(rpc_service.next(&queue)==PH_LIFECYCLE_SERVICE_IDLE);
 for(unsigned i=0;i<4;i++) {
  size_t size=0;assert(gpud_gpu_rpc_next_event(&rpc,reply,sizeof(reply),&size)==1);
  struct ph_gpu_drm_event_message event;
  assert(!ph_gpu_drm_event_decode(reply,size,9,&event));
  assert(event.sequence==i+1 && event.session_id==event_session && event.data_size==64 && !event.fences);
  assert(ph_gpu_event_load_u32(event.data+4)==32 && ph_gpu_event_load_u32(event.data+36)==32);
  assert(!rpc.error && !rpc.failure_line);
 }
 size_t size=0;assert(!gpud_gpu_rpc_next_event(&rpc,reply,sizeof(reply),&size));
 assert(wake_read==wake_write && !rpc.event_notification && !rpc.error);
 /* Completion after repost returned, without an available-ring wake:
  * the Linux callback itself makes the already posted buffer runnable. */
 for(unsigned i=0;i<8;i++) {
  event_backend_ready(&ipc);
  assert(gpud_gpu_rpc_next_event(&rpc,reply,sizeof(reply),&size)==1);
  struct ph_gpu_drm_event_message event;
  assert(!ph_gpu_drm_event_decode(reply,size,9,&event));
  assert(event.sequence==i+5 && event.session_id==event_session && event.data_size==64);
  assert(!gpud_gpu_rpc_next_event(&rpc,reply,sizeof(reply),&size));
  assert(!rpc.error && wake_read==wake_write);
 }
 assert(queue.channel.lanes[GPUD_GPU_QUEUE_EVENT].consumed==12);
 assert(frontend.lanes[GPUD_GPU_QUEUE_EVENT].consumed==12);
 assert(!rpc_service.stop(rpc_service.context));event_mode=0;
 assert(!ph_gpu_queue_finish(&queue));
 printf("RPC events %s display completion + 3 immediate/8 delayed reposts + two-record batches PASS\n",order?"after":"before");
}

/* Deliberately injected packets classify line-74 candidates; they do not
 * claim that an out-of-lane wake can arise from serialized normal calls. */
static void wait_wake_notification_matrix(void) {
 struct gpud_gpu_channel channel={0};struct ph_ipc ipc={.generation=9};
 for(unsigned i=0;i<GPUD_GPU_QUEUE_COUNT;i++)channel.lanes[i].queue.used_notification_id=2*i+2;
 for(unsigned id=1;id<=8;id++) {
  struct gpud_gpu_rpc rpc={.ipc=&ipc,.channel=&channel};
  wake_read=wake_write=0;command_pending=(struct ph_ipc_packet){0};event_mode=1;
  wake_fifo[wake_write++]=(struct ph_ipc_packet){.operation=PH_GPU_QUEUE_NOTIFY,.generation=9,.value=id};
  if(id==2)wake_fifo[wake_write++]=(struct ph_ipc_packet){.operation=PH_GPU_QUEUE_NOTIFY,.generation=9,.value=8};
  int result=wait_wake(&rpc,8,2000);
  if(id==2 || id==8)assert(!result && !rpc.error);
  else assert(result==-EPROTO && rpc.error==-EPROTO && rpc.failure_line);
  assert(rpc.event_notification==(id==2));
 }
 event_mode=0;
 puts("RPC execution wake ID matrix: event 2/expected 8 accepted; other lane 4/6 and available 1/3/5/7 rejected PASS");
}

int main(void) {
 wait_wake_notification_matrix();
 event_rpc_ordering(0);event_rpc_ordering(1);
 sync_file_codec_admission();
 for(unsigned op=0;op<3;++op) for(unsigned bad=0;bad<8;++bad)sync_file_rpc_roundtrip(op,bad);
 sync_file_rpc_roundtrip(0,8);sync_file_rpc_roundtrip(0,9);sync_file_rpc_roundtrip(1,10);
 puts("Sync_file owner RPC + native control queue: export/import/release, preserved 64-bit flags, no file session, errno and forged reply rejection PASS");
 for(unsigned i=0;i<8;++i)rpc_roundtrip(i);
 puts("DMA-BUF owner RPC + native control queue: flags/errno, no file session, forged reply rejection PASS");
 puts("RPC diagnostics: actual malformed-reply first line/error survive subsequent helper and public calls; driver errno leaves diagnostics clear PASS");
}
