#include <assert.h>
#include <errno.h>
#include <stdio.h>
struct gpud_drm_service { int error,prefer_prime,prefer_connection,endpoint_fd; };
static int prime_result,connection_result,public_result,prime_calls,connection_calls,public_calls;
static int receive_prime_cpu_sync(struct gpud_drm_service *s){(void)s;prime_calls++;return prime_result;}
static int receive_connection(struct gpud_drm_service *s){(void)s;connection_calls++;return connection_result;}
static int receive_request(struct gpud_drm_service *s,int fd,void *c){(void)s;(void)fd;(void)c;public_calls++;return public_result;}
#include "receive-actual.inc"
int main(void){
 struct gpud_drm_service s={.endpoint_fd=16};
 prime_result=connection_result=public_result=0;
 for(int i=0;i<1000;i++)assert(!gpud_drm_service_receive(&s));
 assert(prime_calls==500&&connection_calls==250&&public_calls==250);
 prime_calls=connection_calls=public_calls=0;
 prime_result=-EAGAIN;connection_result=public_result=0;
 for(int i=0;i<1000;i++)assert(!gpud_drm_service_receive(&s));
 assert(connection_calls==500&&public_calls==500);
 prime_calls=connection_calls=public_calls=0;
 prime_result=connection_result=public_result=-EAGAIN;
 assert(gpud_drm_service_receive(&s)==-EAGAIN);
 assert(prime_calls==1&&connection_calls==1&&public_calls==1);
 s.prefer_prime=1;prime_result=-EIO;prime_calls=connection_calls=public_calls=0;
 assert(gpud_drm_service_receive(&s)==-EIO&&prime_calls==1&&!connection_calls&&!public_calls);
 s.error=-EPIPE;assert(gpud_drm_service_receive(&s)==-EPIPE);
 assert(gpud_drm_service_receive(0)==-EINVAL);
 puts("actual receive: bounded PRIME/connection/public fairness, fallback, fatal error PASS");
}
