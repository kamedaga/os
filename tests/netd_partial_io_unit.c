/* Real netd backend and unmodified libuinet; reuse the host boundary fixture. */
#define main reconfiguration_fixture_main
#include "netd_libuinet_reconfiguration.c"
#undef main

static void initialize_partial_fixture(void)
{
    struct netd_boot_config config = {0};
    memcpy(config.ipv4.address, old_ip, 4);
    memset(config.ipv4.netmask, 255, 3);
    memcpy(config.ipv4.gateway, peer_ip, 4);
    CHECK(netd_network_config_init(&config.ipv4) == 0);
    netd_link_init(receive, NULL);
    struct netd_link_info info = {.generation=1,
        .interface_id=NETD_LINK_ID_PRIMARY,.mtu=1500,.carrier=1};
    memcpy(info.mac,local_mac,6);
    struct netd_link_ops ops={.send=capture};
    CHECK(netd_link_attach(&info,&ops,NULL)==0);
    struct netd_runtime runtime={.cfg=&config};
    CHECK(netd_libuinet_start(&runtime)==0);
    pump(5);
}

static uint32_t collect_stream(uint16_t port, uint8_t *bytes, uint8_t *seen,
    size_t length, uint32_t *base_ptr)
{
    uint32_t base=*base_ptr,end=base;
    for(size_t i=0;i<captured_count;++i) {
        const uint8_t *b=captured[i].bytes;
        if(captured[i].size<54 || b[23]!=6 || b[36]!=(port>>8) || b[37]!=(port&255)) continue;
        const size_t ih=(b[14]&15)*4u,th=(b[14+ih+12]>>4)*4u;
        const size_t total=((size_t)b[16]<<8)|b[17];
        if(total<ih+th || total+14>captured[i].size) continue;
        CHECK(checksum(b+14,ih,0)==0);
        uint32_t pseudo=6+(uint32_t)(total-ih);
        for(unsigned j=26;j<34;j+=2)pseudo+=(unsigned)b[j]*256+b[j+1];
        CHECK(checksum(b+14+ih,total-ih,pseudo)==0);
        const size_t n=total-ih-th;
        const uint32_t seq=get32(b+14+ih+4);
        if(!n)continue;
        if(!base) {base=seq;end=base;}
        const uint32_t offset=seq-base;
        CHECK(offset<=length && n<=length-offset);
        for(size_t j=0;j<n;++j) {
            if(seen[offset+j])CHECK(bytes[offset+j]==b[14+ih+th+j]);
            bytes[offset+j]=b[14+ih+th+j];seen[offset+j]=1;
        }
        if(seq+(uint32_t)n>end)end=seq+(uint32_t)n;
    }
    *base_ptr=base;
    return end;
}

static void inject_data(uint16_t port,uint32_t sequence,uint32_t ack,
    const uint8_t *data,size_t length,int corrupt)
{
    uint8_t b[1600]={0};CHECK(length<=sizeof(b)-54);
    memcpy(b,local_mac,6);memcpy(b+6,peer_mac,6);put16(b+12,0x800);
    b[14]=0x45;put16(b+16,40+length);b[22]=64;b[23]=6;
    memcpy(b+26,peer_ip,4);memcpy(b+30,old_ip,4);
    put16(b+24,checksum(b+14,20,0));put16(b+34,port);put16(b+36,2222);
    put32(b+38,sequence);put32(b+42,ack);b[46]=0x50;b[47]=0x18;put16(b+48,65535);
    memcpy(b+54,data,length);uint32_t sum=6+20+length;
    for(unsigned i=26;i<34;i+=2)sum+=(unsigned)b[i]*256+b[i+1];
    put16(b+50,checksum(b+34,20+length,sum));
    if(corrupt)b[54]^=1;
    CHECK(netd_link_receive(1,b,54+length)==0);pump(5);
}

int main(void)
{
    setbuf(stdout,NULL);initialize_partial_fixture();
    /* Cover the shared send/receive normalization, including signal errors
     * and corrupt library residuals; no-progress retries must remain EAGAIN. */
    const int errors[]={UINET_EWOULDBLOCK,UINET_EAGAIN,UINET_EINTR,-1};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        size_t n=99;CHECK(netd_libuinet_io_result(errors[i],100,40,&n)==0&&n==60);
        CHECK(netd_libuinet_io_result(errors[i],100,100,&n)<0&&n==0);
    }
    size_t n=99;CHECK(netd_libuinet_io_result(0,100,-1,&n)==-EIO&&n==0);
    CHECK(netd_libuinet_io_result(0,100,101,&n)==-EIO&&n==0);
    CHECK(netd_libuinet_io_result(UINET_ECONNRESET,100,40,&n)==-UINET_ECONNRESET&&n==0);

    struct test_socket listener=open_socket(NETD_SOCK_STREAM);
    const uint8_t any[4]={0};bind_socket(listener,any,2222);
    CHECK(netd_libuinet_socket_listen(listener.handle,8)==0);
    handshake(old_ip,42001);struct test_socket accepted=accept_socket(listener,old_ip,42001);
    struct uinet_socket *so=netd_libuinet_api_socket_find(accepted.handle)->socket;
    int capacity=8192;CHECK(uinet_sosetsockopt(so,UINET_SOL_SOCKET,UINET_SO_SNDBUF,&capacity,sizeof capacity)==0);
    uint8_t data[16384],original[16384],wire[16384]={0},seen[16384]={0};
    for(size_t i=0;i<sizeof data;++i)data[i]=(uint8_t)(i^(i>>8));
    memcpy(original,data,sizeof data);captured_count=0;
    size_t sent=0;int status=netd_libuinet_socket_send(accepted.handle,data,sizeof data,0,0,0,&sent);
    printf("partial send status=%d accepted=%zu remaining=%zu\n",status,sent,sizeof data-sent);
    CHECK(status==0&&sent==8192);
    size_t again=99;CHECK(netd_libuinet_socket_send(accepted.handle,data+sent,sizeof data-sent,0,0,0,&again)==-EAGAIN&&again==0);
    uint32_t base=0;
    uint32_t end=collect_stream(42001,wire,seen,sizeof wire,&base);
    for(size_t i=0;i<sent;++i)if(seen[i])CHECK(wire[i]==original[i]);
    struct uinet_tcpstat before={0},after={0};uinet_gettcpstat(uinet_instance_default(),&before);
    /* Accepted bytes must belong to TCP, even if the caller reuses its RPC
     * page before an ACK or retransmission. Verify retransmitted payloads. */
    memset(data,0xcc,sent);pump(4000);uinet_gettcpstat(uinet_instance_default(),&after);
    printf("retransmit before=%lu after=%lu captured=%zu end=%u base=%u\n",before.tcps_sndrexmitpack,after.tcps_sndrexmitpack,captured_count,end,base);
    CHECK(after.tcps_sndrexmitpack>before.tcps_sndrexmitpack);
    end=collect_stream(42001,wire,seen,sizeof wire,&base);
    for(unsigned i=0;i<20;++i) {
        tcp(old_ip,42001,0x10,1001,end);
        end=collect_stream(42001,wire,seen,sizeof wire,&base);
        if(end==base+sent)break;
    }
    CHECK(end==base+sent);
    tcp(old_ip,42001,0x10,1001,end);
    size_t tail=0;CHECK(netd_libuinet_socket_send(accepted.handle,data+sent,sizeof data-sent,0,0,0,&tail)==0);
    CHECK(sent+tail==sizeof data);
    end=collect_stream(42001,wire,seen,sizeof wire,&base);
    for(unsigned i=0;i<20;++i) {
        tcp(old_ip,42001,0x10,1001,end);
        end=collect_stream(42001,wire,seen,sizeof wire,&base);
        if(end==base+sizeof wire)break;
    }
    for(size_t i=0;i<sizeof wire;++i)CHECK(seen[i]&&wire[i]==original[i]);
    tcp(old_ip,42001,0x10,1001,end);
    puts("PASS partial progress, zero-progress EAGAIN, caller-page reuse, real retransmit, exact TCP stream and TX checksums");

    uinet_gettcpstat(uinet_instance_default(),&before);
    inject_data(42001,1001,end,original,1000,1);
    uinet_gettcpstat(uinet_instance_default(),&after);
    CHECK(after.tcps_rcvbadsum==before.tcps_rcvbadsum+1);
    uint8_t output[1000];uint32_t addr;uint16_t port;size_t got;
    CHECK(netd_libuinet_socket_recv(accepted.handle,output,sizeof output,0,&got,&addr,&port)==-EAGAIN&&got==0);
    inject_data(42001,1001,end,original,1000,0);
    CHECK(netd_libuinet_socket_recv(accepted.handle,output,sizeof output,0,&got,&addr,&port)==0);
    CHECK(got==sizeof output&&!memcmp(output,original,got));
    puts("PASS RX invalid checksum rejected; valid payload delivered exactly once");
    return 0;
}
