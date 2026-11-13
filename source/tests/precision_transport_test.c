#define _GNU_SOURCE
#define UB_HOST_TEST 1
#define UB_PC_SOURCE_PEER "precision_transport_test"
#define UB_PC_CLIENT_PEER "precision_transport_test"
#include <stdint.h>
static char test_address[64];
static int test_clock_failure;
static int test_transport_now_ms(uint64_t *ms);
#define UB_PC_NOW_MS test_transport_now_ms
#define UB_PC_SOCKET_NAME test_address
static void test_source_backoff(void);
#define UB_PC_SOURCE_BACKOFF() test_source_backoff()
#include "../modules/external_subtitles/precision_transport.h"
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#include <dirent.h>
#include <sys/eventfd.h>

struct shared {int calls,mode,backoffs,paused,release;uint32_t generation;};static struct shared *shared;
static int source_event_fd;
static int test_paused(uint32_t *generation) {
 *generation=__atomic_load_n(&shared->generation,__ATOMIC_ACQUIRE);
 return __atomic_load_n(&shared->paused,__ATOMIC_ACQUIRE);
}
static void set_paused(int paused) {
 uint64_t one=1;__atomic_store_n(&shared->paused,paused,__ATOMIC_RELEASE);
 __atomic_add_fetch(&shared->generation,1,__ATOMIC_RELEASE);
 assert(write(source_event_fd,&one,sizeof(one))==sizeof(one));
}
static int test_transport_now_ms(uint64_t *ms) {
 return test_clock_failure?-1:ub_payload_now_ms(ms);
}
static int count_fds(void) {
 DIR *dir=opendir("/proc/self/fd");int count=0;assert(dir);
 while(readdir(dir))count++;
 closedir(dir);return count;
}
static void test_unexpected_rights(void) {
 int pair[2],pipes[2],one=1,passed[20];struct ub_pc_record record;
 struct ub_pc_credentials peer;char payload='x';struct iovec io={&payload,1};
 union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(passed))];} control;
 struct msghdr msg;assert(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,pair));
 assert(!ub_pc_setsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&one,sizeof(one)));
 assert(!pipe(pipes));for(int i=0;i<20;i++)passed[i]=pipes[0];
 for(int copies=1;copies<=20;copies+=19) {
  memset(&msg,0,sizeof(msg));memset(&control,0,sizeof(control));
  msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control.bytes;
  msg.msg_controllen=CMSG_SPACE((size_t)copies*sizeof(int));
  struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
  c->cmsg_len=CMSG_LEN((size_t)copies*sizeof(int));memcpy(CMSG_DATA(c),passed,(size_t)copies*sizeof(int));
  int before=count_fds();assert(sendmsg(pair[0],&msg,0)==1);
  assert(ub_pc_receive(pair[1],&record,sizeof(record),MSG_DONTWAIT,0,0,&peer)==-2);
  assert(count_fds()==before); /* Includes installed fds before MSG_CTRUNC rejection. */
 }
 close(pipes[0]);close(pipes[1]);close(pair[0]);close(pair[1]);
}
static void test_source_backoff(void) {
 __atomic_add_fetch(&shared->backoffs,1,__ATOMIC_RELEASE);usleep(250000);
}
static int getter(uint32_t words[2]) {
 int mode=__atomic_load_n(&shared->mode,__ATOMIC_ACQUIRE);
 __atomic_add_fetch(&shared->calls,1,__ATOMIC_RELEASE);
 if(mode==1)usleep(300000);
 if(mode==3)usleep(80000);
 if(mode==5)while(!__atomic_load_n(&shared->release,__ATOMIC_ACQUIRE))usleep(1000);
 words[0]=10*45000+12345;words[1]=0;
 return mode==2?-1:0;
}
static uint64_t now_ms(void){uint64_t now;assert(!ub_payload_now_ms(&now));return now;}
static int checked_step(struct ub_pc_client *client,uint64_t nonce,int active,uint64_t *ticks) {
 uint64_t begin=now_ms();int result=ub_pc_client_step(client,nonce,active,10,ticks);
 assert(now_ms()-begin<50);return result;
}
static int wait_sample(struct ub_pc_client *client,uint64_t nonce,uint64_t *ticks,unsigned timeout) {
 uint64_t end=now_ms()+timeout;
 do {if(checked_step(client,nonce,1,ticks))return 1;usleep(2000);}while(now_ms()<end);
 return 0;
}
static int wait_paused(struct ub_pc_client *client,uint64_t nonce,int paused,unsigned timeout) {
 uint64_t ticks,end=now_ms()+timeout;
 do {checked_step(client,nonce,1,&ticks);if((int)client->paused==paused)return 1;usleep(2000);}while(now_ms()<end);
 return 0;
}
int main(void) {
 struct ub_pc_client client;struct sockaddr_un address,local;struct ub_pc_request bad;
 uint64_t ticks=0;int raw,status,baseline,occupied;pid_t child;
 test_unexpected_rights();
 snprintf(test_address,sizeof(test_address),"ub820.srt.clock.test.%u",(unsigned)getpid());
 shared=mmap(0,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 assert(shared!=MAP_FAILED);memset(shared,0,sizeof(*shared));
 source_event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(source_event_fd>=0);
 /* Simulate the old apl_bdv retaining the abstract listener during overlap.
  * The new source remains resident, backs off, then binds after release.
  */
 socklen_t len=ub_pc_address(&address);occupied=ub_pc_socket(1);assert(occupied>=0);
 assert(!bind(occupied,(struct sockaddr *)&address,len));
 child=fork();assert(child>=0);
 if(!child){close(occupied);ub_precision_source_serve(getter,test_paused,source_event_fd);_exit(2);}
 usleep(620000);assert(waitpid(child,&status,WNOHANG)==0);
 assert(!__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE));
 int retries=__atomic_load_n(&shared->backoffs,__ATOMIC_ACQUIRE);
 assert(retries>=2&&retries<=4); /* 250ms sleep, not a busy retry loop. */
 close(occupied);usleep(350000);ub_pc_client_init(&client);
 /* Invalid input is consumed without touching the native getter. */
 raw=ub_pc_socket(1);assert(raw>=0);memset(&local,0,sizeof(local));local.sun_family=AF_UNIX;
 assert(!bind(raw,(struct sockaddr *)&local,offsetof(struct sockaddr_un,sun_path)));
 ub_pc_request_pack(&bad,42,1);bad.magic=0;
 assert(sendto(raw,&bad,sizeof(bad),MSG_DONTWAIT,(struct sockaddr *)&address,len)==sizeof(bad));
 close(raw);usleep(30000);assert(!__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE));
 assert(!checked_step(&client,1,0,&ticks));usleep(30000);
 assert(!__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE));
 assert(wait_sample(&client,1,&ticks,1500));assert(ticks==10*45000+12345);
 /* Rapid inactive->active keeps the same endpoint and immediately requests a
  * new lifecycle sample, rather than waiting for the old 1s bind retry gate. */
 int retained_fd=client.fd;
 assert(!checked_step(&client,2,0,&ticks));assert(client.fd==retained_fd);
 usleep(50000); /* A previously in-flight reply may arrive after becoming inactive. */
 struct sockaddr_un client_address;socklen_t client_length=sizeof(client_address);
 assert(!getsockname(client.fd,(struct sockaddr *)&client_address,&client_length));
 raw=ub_pc_socket(1);assert(raw>=0);struct ub_pc_record late;
 ub_pc_pack(&late,(uint32_t)getpid(),123,1,1,1,now_ms(),10*45000,1);
 assert(sendto(raw,&late,sizeof(late),MSG_DONTWAIT,(struct sockaddr *)&client_address,client_length)==sizeof(late));
 struct pollfd ready={client.fd,POLLIN,0};assert(poll(&ready,1,0)==1);
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 assert(!checked_step(&client,2,0,&ticks));ready.revents=0;assert(poll(&ready,1,0)==0);
 for(int i=0;i<10;i++)assert(!checked_step(&client,2,0,&ticks));
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline);
 /* Clock errors must also drain retained endpoints and cancel pending work. */
 assert(sendto(raw,&late,sizeof(late),MSG_DONTWAIT,(struct sockaddr *)&client_address,client_length)==sizeof(late));
 test_clock_failure=1;client.pending=1;assert(!checked_step(&client,2,1,&ticks));
 assert(!client.pending);ready.revents=0;assert(poll(&ready,1,0)==0);
 test_clock_failure=0;close(raw);
 uint64_t resume=now_ms();
 assert(wait_sample(&client,3,&ticks,180));assert(now_ms()-resume<180);
 assert(client.fd==retained_fd&&ticks==10*45000+12345);
 /* The getter may block on its own worker; client calls remain bounded. */
 __atomic_store_n(&shared->mode,3,__ATOMIC_RELEASE);
 ub_pc_client_reset(&client,2);assert(wait_sample(&client,2,&ticks,1500));
 assert(ticks==10*45000+12345);
 /* Let an old-epoch request enter the slow getter, then change lifecycle.
  * Its reply must never become a sample in the new lifecycle. */
 __atomic_store_n(&shared->mode,1,__ATOMIC_RELEASE);
 ub_pc_client_reset(&client,3);baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 uint64_t until=now_ms()+500;
 while(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline&&now_ms()<until) {
  assert(!checked_step(&client,3,1,&ticks));usleep(2000);
 }
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)>baseline);
 assert(!checked_step(&client,4,1,&ticks));
 until=now_ms()+450;
 while(now_ms()<until){assert(!checked_step(&client,4,1,&ticks));usleep(2000);}
 /* Source errors invalidate rather than inventing a position. */
 __atomic_store_n(&shared->mode,2,__ATOMIC_RELEASE);
 ub_pc_client_reset(&client,5);assert(!wait_sample(&client,5,&ticks,500));
 __atomic_store_n(&shared->mode,0,__ATOMIC_RELEASE);
 ub_pc_client_reset(&client,6);assert(wait_sample(&client,6,&ticks,1500));
 /* Pause uses the explicit source state, never a stationary-clock heuristic.
  * No further getter call occurs even if step is called repeatedly. */
 set_paused(1);assert(wait_paused(&client,6,1,1500));
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 until=now_ms()+300;
 while(now_ms()<until){checked_step(&client,6,1,&ticks);assert(client.paused);usleep(2000);}
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline);
 /* A transient clock failure may consume the resume notification. It must
  * revoke pause suspension so the caller retries instead of sleeping forever. */
 set_paused(0);ready.revents=0;assert(poll(&ready,1,1000)==1);
 test_clock_failure=1;assert(!checked_step(&client,6,1,&ticks));
 assert(!client.paused&&!client.state.paused&&!client.pending);
 ready.revents=0;assert(poll(&ready,1,0)==0);test_clock_failure=0;
 assert(wait_sample(&client,6,&ticks,1500));
 set_paused(1);assert(wait_paused(&client,6,1,1500));
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 /* Fill the destination queue: a resume notification must survive EAGAIN,
  * then wake through POLLOUT after junk records have been drained. */
 raw=ub_pc_socket(1);assert(raw>=0);memset(&late,0,sizeof(late));int filled=0;
 for(int i=0;i<4096;i++) {
  ssize_t sent=sendto(raw,&late,sizeof(late),MSG_DONTWAIT,(struct sockaddr *)&client_address,client_length);
  if(sent<0){assert(errno==EAGAIN||errno==EWOULDBLOCK);filled=1;break;}
 }
 assert(filled);set_paused(0);usleep(50000);
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline);
 assert(wait_paused(&client,6,0,1500));close(raw);
 assert(wait_sample(&client,6,&ticks,1500));
 /* Pause/resume/pause while an 80ms query owns MM is coalesced safely:
  * the reply describes the final explicit state, not its pre-query state. */
 __atomic_store_n(&shared->mode,3,__ATOMIC_RELEASE);ub_pc_client_wake(&client);
 uint32_t before_query_sequence=client.state.sequence;
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);until=now_ms()+500;
 while(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline&&now_ms()<until) {
  checked_step(&client,6,1,&ticks);usleep(2000);
 }
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)>baseline);
 set_paused(1);set_paused(0);set_paused(1);
 until=now_ms()+1500;
 while(client.state.sequence==before_query_sequence&&now_ms()<until){checked_step(&client,6,1,&ticks);usleep(2000);}
 assert(client.state.sequence!=before_query_sequence&&!client.paused);
 /* Only a second query with stable paused state+generation can suspend. */
 assert(wait_paused(&client,6,1,1500));
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);usleep(150000);
 checked_step(&client,6,1,&ticks);assert(client.paused);
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline);
 __atomic_store_n(&shared->mode,0,__ATOMIC_RELEASE);
 /* A local lifecycle/native wake invalidates an already pending request.
  * Its delayed PAUSED reply must not undo that wake before the new query. */
 __atomic_store_n(&shared->release,0,__ATOMIC_RELEASE);
 __atomic_store_n(&shared->mode,5,__ATOMIC_RELEASE);baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 ub_pc_client_wake(&client);checked_step(&client,6,1,&ticks);until=now_ms()+500;
 while(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline&&now_ms()<until)usleep(2000);
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)>baseline);
 ub_pc_client_wake(&client);__atomic_store_n(&shared->mode,0,__ATOMIC_RELEASE);
 __atomic_store_n(&shared->release,1,__ATOMIC_RELEASE);usleep(20000);
 checked_step(&client,6,1,&ticks);assert(!client.paused);
 assert(wait_paused(&client,6,1,1500));
 /* Root's wake-before-Off invalidates old request sequences, but unsubscribe
  * still matches the authenticated endpoint and never queries native MM. */
 ub_pc_client_wake(&client);
 /* Stop queries, drain the single in-flight request, then verify idle remains idle. */
 assert(!checked_step(&client,7,0,&ticks));usleep(250000);
 baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 set_paused(0);usleep(50000);ready.revents=0;assert(poll(&ready,1,0)==0);
 for(int i=0;i<10;i++){assert(!checked_step(&client,7,0,&ticks));usleep(5000);}
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==baseline);
 /* A restarted datagram source does not promise a HUP. Native lifecycle/Draw
  * wake is the recovery mechanism, and must permit a fresh producer cookie. */
 set_paused(1);assert(wait_paused(&client,8,1,1500));
 assert(!kill(child,SIGTERM));assert(waitpid(child,&status,0)==child);
 assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGTERM);
 child=fork();assert(child>=0);
 if(!child){ub_precision_source_serve(getter,test_paused,source_event_fd);_exit(2);}
 usleep(50000);ub_pc_client_wake(&client);until=now_ms()+1500;
 while(client.state.pid!=(uint32_t)child&&now_ms()<until){checked_step(&client,8,1,&ticks);usleep(2000);}
 assert(client.state.pid==(uint32_t)child&&client.paused);
 ub_pc_client_close(&client);assert(!kill(child,SIGTERM));assert(waitpid(child,&status,0)==child);
 /* Without a source wake descriptor there is no pause suspension; queries
  * continue so an eventfd allocation failure cannot strand a paused client. */
 child=fork();assert(child>=0);
 if(!child){ub_precision_source_serve(getter,test_paused,-1);_exit(2);}
 usleep(50000);baseline=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 assert(wait_sample(&client,9,&ticks,1500));until=now_ms()+150;
 while(now_ms()<until){checked_step(&client,9,1,&ticks);assert(!client.paused);usleep(2000);}
 assert(__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)>baseline+2);
 ub_pc_client_close(&client);assert(!kill(child,SIGTERM));assert(waitpid(child,&status,0)==child);
 close(source_event_fd);munmap(shared,sizeof(*shared));
 puts("precision_transport_test passed");return 0;
}
