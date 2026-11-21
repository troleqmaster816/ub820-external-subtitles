/* Real source and client over a datagram socket. A native notification that
 * lands during GET (a small backward seek) must not let the pre-seek sample
 * through; an ordinary generation change (1 Hz clock callbacks) during one
 * query must not cost the sample; changes during both queries give no
 * sample. Based on the follow-up review's clock_state_change.c. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#define UB_PC_SOURCE_PEER "precision_seek_race_test"
#define UB_PC_CLIENT_PEER "precision_seek_race_test"
static char endpoint[64];
#define UB_PC_SOCKET_NAME endpoint
#include "../modules/external_subtitles/precision_transport.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>

struct shared {uint32_t generation,calls,mode,state;};static struct shared *shared;
/* mode 0: seek during the first GET only (10.5 s -> 10.1 s).
 * mode 1: the first GET of every request crosses a generation change,
 *         position advances 20 ms per GET.
 * mode 2: every GET crosses a generation change. */
static int sample(uint32_t words[2]){
 uint32_t call=__atomic_fetch_add(&shared->calls,1,__ATOMIC_ACQ_REL);
 if(!__atomic_load_n(&shared->mode,__ATOMIC_ACQUIRE)){
  words[0]=10*45000+(call?4500:22500);words[1]=0;
  if(!call)__atomic_add_fetch(&shared->generation,1,__ATOMIC_RELEASE);
 }else{
  words[0]=10*45000+call*900;words[1]=0;
  if(__atomic_load_n(&shared->mode,__ATOMIC_ACQUIRE)==2||!(call&1))__atomic_add_fetch(&shared->generation,1,__ATOMIC_RELEASE);
 }
 return 0;
}
static int paused(uint32_t *generation){
 *generation=__atomic_load_n(&shared->generation,__ATOMIC_ACQUIRE);return (int)__atomic_load_n(&shared->state,__ATOMIC_ACQUIRE);
}
static pid_t serve(int fd){
 pid_t server=fork();assert(server>=0);
 if(!server){ub_precision_source_serve(sample,paused,fd);_exit(1);}
 return server;
}
static void stop(pid_t server){assert(!kill(server,SIGTERM));assert(waitpid(server,0,0)==server);}
int main(void){
 struct ub_pc_client client;uint64_t ticks=0;unsigned cases=0;
 snprintf(endpoint,sizeof endpoint,"ub820.seek.race.%u",(unsigned)getpid());
 shared=mmap(0,sizeof *shared,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);
 memset(shared,0,sizeof *shared);shared->generation=11;shared->state=1;
 int fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(fd>=0);
 /* 1. Paused seek from 10.5 s to 10.1 s completes during the first GET. */
 pid_t server=serve(fd);ub_pc_client_init(&client);
 for(unsigned i=0;i<1000&&!client.state.have_packet;i++){ub_pc_client_step(&client,1,1,10,&ticks);usleep(1000);}
 assert(client.state.have_packet&&client.state.valid&&client.state.native_generation==12);
 assert(client.state.ticks==10*45000+4500);                 /* never the pre-seek 10.5 s */
 assert(ub_pc_client_step(&client,1,1,10,&ticks)&&ticks==10*45000+4500);cases++;
 assert(client.paused&&__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE)==2);cases++; /* the re-query was stable */
 ub_pc_client_close(&client);stop(server);
 /* 2. Ordinary playback where every request crosses a clock callback: every
  *    request still yields an accepted, advancing sample (one re-query each). */
 memset(shared,0,sizeof *shared);shared->generation=1;shared->mode=1;shared->state=0;
 server=serve(fd);ub_pc_client_init(&client);
 unsigned accepted=0;uint64_t previous=0;uint32_t last_sequence=0;
 for(unsigned i=0;i<3000&&accepted<20;i++){
  ub_pc_client_step(&client,2,1,10,&ticks);
  if(client.state.have_packet&&client.state.sequence!=last_sequence){
   last_sequence=client.state.sequence;assert(client.state.valid&&client.state.ticks>previous);
   previous=client.state.ticks;accepted++;
  }
  usleep(1000);
 }
 uint32_t calls=__atomic_load_n(&shared->calls,__ATOMIC_ACQUIRE);
 assert(accepted==20&&calls<=2*accepted+2);cases++;
 ub_pc_client_close(&client);stop(server);
 /* 3. The state changes again during the repeated query: neither sample can
  *    be tied to a generation, so the reply is invalid and none is used. */
 memset(shared,0,sizeof *shared);shared->generation=1;shared->mode=2;shared->state=0;
 server=serve(fd);ub_pc_client_init(&client);
 unsigned replies=0,used=0;last_sequence=0;
 for(unsigned i=0;i<600&&replies<5;i++){
  if(ub_pc_client_step(&client,2,1,10,&ticks))used++;
  if(client.state.have_packet&&client.state.sequence!=last_sequence){last_sequence=client.state.sequence;assert(!client.state.valid);replies++;}
  usleep(1000);
 }
 assert(replies==5&&!used);cases++;
 ub_pc_client_close(&client);stop(server);close(fd);munmap(shared,sizeof *shared);
 printf("precision_seek_race_test passed: %u cases; no pre-seek sample after a change during GET, ordinary changes keep every sample, repeated changes give none\n",cases);
 return 0;
}
