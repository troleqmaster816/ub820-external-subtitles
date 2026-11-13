#ifndef UB_SUBTITLE_PRECISION_TRANSPORT_H
#define UB_SUBTITLE_PRECISION_TRANSPORT_H
#include "precision_clock.h"
#include "../../core/payload_time.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#ifndef UB_PC_NOW_MS
#define UB_PC_NOW_MS ub_payload_now_ms
#endif
#ifdef UB_HOST_TEST
#define ub_pc_recvmsg recvmsg
#define ub_pc_setsockopt setsockopt
#else
/* The native libc is 2.20: modern ARM headers redirect these two interfaces
 * to time64 wrappers even though we only request credentials, not timestamps. */
extern ssize_t ub_pc_recvmsg(int,struct msghdr *,int) __asm__("recvmsg");
extern int ub_pc_setsockopt(int,int,int,const void *,socklen_t) __asm__("setsockopt");
#endif
#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 2
#endif
#ifndef SO_PASSCRED
#define SO_PASSCRED 16
#endif
#ifndef UB_PC_SOCKET_NAME
#define UB_PC_SOCKET_NAME "ub820.srt.clock.v2"
#endif
#ifndef UB_PC_SOURCE_PEER
#define UB_PC_SOURCE_PEER "apl_meda_bdplay"
#endif
#ifndef UB_PC_CLIENT_PEER
#define UB_PC_CLIENT_PEER "apl_bdv"
#endif
#ifndef UB_PC_SOURCE_UID
#define UB_PC_SOURCE_UID ((uint32_t)geteuid())
#endif
#define UB_PC_MAX_AGE_MS 200u
#define UB_PC_PERIOD_MS 20u
#define UB_PC_DRAIN_LIMIT 32u
#ifndef UB_PC_SOURCE_BACKOFF
#define UB_PC_SOURCE_BACKOFF() ((void)usleep(250000u))
#endif
struct ub_pc_credentials {int32_t pid;uint32_t uid,gid;};
static inline socklen_t ub_pc_address(struct sockaddr_un *a) {
 size_t n=strlen(UB_PC_SOCKET_NAME);memset(a,0,sizeof(*a));a->sun_family=AF_UNIX;
 if(n>sizeof(a->sun_path)-2)return 0;
 memcpy(a->sun_path+1,UB_PC_SOCKET_NAME,n);
 return (socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+n);
}
static inline int ub_pc_peer_name(uint32_t pid,const char *wanted) {
 char path[64],digits[10],exe[512];ssize_t n;const char *base;size_t used=6,count=0;
 if(!pid)return 0;
 memcpy(path,"/proc/",6);
 do {uint32_t quotient=(uint32_t)(((uint64_t)pid*0xcccccccdULL)>>35);
  digits[count++]=(char)('0'+pid-quotient*10u);pid=quotient;
 }while(pid);
 while(count)path[used++]=digits[--count];
 memcpy(path+used,"/exe",5);
 n=readlink(path,exe,sizeof(exe)-1);if(n<=0||n==(ssize_t)sizeof(exe)-1)return 0;
 exe[n]=0;base=strrchr(exe,'/');return base&&strcmp(base+1,wanted)==0;
}
/* MSG_TRUNC detects oversized records even if the first bytes look valid.
 * Credentials are supplied by the kernel; missing/truncated ancillary data
 * fails closed. A short datagram is consumed but cannot be accepted.
 */
static inline ssize_t ub_pc_receive(int fd,void *record,size_t size,int flags,
 struct sockaddr_un *from,socklen_t *from_len,struct ub_pc_credentials *peer) {
 union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(*peer))+CMSG_SPACE(4*sizeof(int))];} control;
 struct iovec io={record,size};struct msghdr msg;struct cmsghdr *c;ssize_t n;int found=0,rights=0;
 memset(&msg,0,sizeof(msg));memset(&control,0,sizeof(control));
 msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control.bytes;msg.msg_controllen=sizeof(control);
 if(from){msg.msg_name=from;msg.msg_namelen=sizeof(*from);}
 n=ub_pc_recvmsg(fd,&msg,flags|MSG_TRUNC|MSG_CMSG_CLOEXEC);
 if(n<0)return n;
 if(from_len)*from_len=msg.msg_namelen;
 for(c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
  if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)) {
   size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);rights=1;
   for(size_t i=0;i<count;i++){int received;memcpy(&received,(unsigned char *)CMSG_DATA(c)+i*sizeof(int),sizeof(int));if(received>=0)close(received);}
  }
  if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_CREDENTIALS&&
     c->cmsg_len==CMSG_LEN(sizeof(*peer))) {memcpy(peer,CMSG_DATA(c),sizeof(*peer));found=1;}
 }
 /* Close every installed descriptor even on a truncated/invalid datagram.
  * The kernel closes descriptors which do not fit in the ancillary buffer. */
 if(!found||rights||(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))){errno=EBADMSG;return -2;}return n;
}
static inline int ub_pc_socket(int nonblock) {
 int one=1,fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|(nonblock?SOCK_NONBLOCK:0),0);
 if(fd<0)return -1;
 if(ub_pc_setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&one,sizeof(one))){close(fd);return -1;}return fd;
}
static inline int ub_pc_request_peer(const struct ub_pc_request *r,ssize_t n,
 const struct ub_pc_credentials *peer) {
 return n>=0&&ub_pc_request_valid(r,(size_t)n)&&peer->pid>0&&
  peer->uid==(uint32_t)UB_PC_SOURCE_UID&&ub_pc_peer_name((uint32_t)peer->pid,UB_PC_SOURCE_PEER);
}
struct ub_pc_subscription {
 int fd,paused,pending;uint32_t pid,generation;
 struct sockaddr_un address;socklen_t length;
 uint64_t nonce;uint32_t request_sequence;
};
static inline void ub_pc_unsubscribe(struct ub_pc_subscription *s) {
 if(s->fd>=0)close(s->fd);
 memset(s,0,sizeof(*s));s->fd=-1;
}
static inline int ub_pc_subscribe(struct ub_pc_subscription *s,
 const struct sockaddr_un *address,socklen_t length,uint32_t pid,
 const struct ub_pc_request *request) {
 if(s->fd<0||s->pid!=pid||s->length!=length||memcmp(&s->address,address,length)) {
  ub_pc_unsubscribe(s);s->fd=ub_pc_socket(1);
  if(s->fd<0)return 0;
  if(connect(s->fd,(const struct sockaddr *)address,length)) {ub_pc_unsubscribe(s);return 0;}
  s->address=*address;s->length=length;s->pid=pid;
 }
 s->nonce=ub_pc_u64(request->client_nonce_lo,request->client_nonce_hi);
 s->request_sequence=request->request_sequence;s->pending=0;return 1;
}
static inline void ub_pc_drain_event(int fd) {
 uint64_t count;
 for(uint32_t i=0;fd>=0&&i<UB_PC_DRAIN_LIMIT;i++)
  if(read(fd,&count,sizeof(count))<0)break;
}
/* Only the source's dedicated worker calls the getter or owns socket I/O.
 * State events never query MM; a paused subscriber receives only a wake token.
 * A connected notification socket lets POLLOUT retain EAGAIN notifications
 * without a timer. Datagram peer death still needs the client's native
 * Draw/lifecycle fallback; it is not a reliable remote-disconnect signal.
 */
static inline void ub_precision_source_serve(int (*get)(uint32_t[2]),
 int (*paused)(uint32_t *),int event_fd) {
 struct sockaddr_un address,from,next;struct ub_pc_request request,candidate;
 struct ub_pc_credentials peer,next_peer;struct ub_pc_record reply;
 struct ub_pc_subscription sub;
 socklen_t address_len=ub_pc_address(&address),from_len=0,next_len=0;
 uint64_t instance,begin,end,last=0;uint32_t sequence=0,words[2],generation,generation_before;
 int fd,state,paused_before;
 if(!get||!address_len)return;
 memset(&sub,0,sizeof(sub));sub.fd=-1;
 while(UB_PC_NOW_MS(&instance))UB_PC_SOURCE_BACKOFF();
 instance=(instance<<16)^(uint32_t)getpid();if(!instance)instance=1;
 /* A previous apl_bdv can retain the abstract address briefly while this
  * process starts. Keep the resident worker alive and retry with backoff;
  * its caller's started flag must never imply a dead listener is recoverable.
  */
 for(;;) {
  fd=ub_pc_socket(0);
  if(fd<0){UB_PC_SOURCE_BACKOFF();continue;}
  if(bind(fd,(struct sockaddr *)&address,address_len)) {
   close(fd);UB_PC_SOURCE_BACKOFF();continue;
  }
  for(;;) {
  generation=0;state=paused?paused(&generation):-1;
  if(sub.fd>=0&&sub.paused&&generation!=sub.generation)sub.pending=1;
  if(sub.pending) {
   if(!UB_PC_NOW_MS(&begin)) {
    ub_pc_pack(&reply,(uint32_t)getpid(),instance,++sequence,sub.nonce,
     sub.request_sequence,begin,0,0);reply.flags=UB_PC_EVENT;reply.native_generation=generation;
    if(send(sub.fd,&reply,sizeof(reply),MSG_DONTWAIT|MSG_NOSIGNAL)==(ssize_t)sizeof(reply)) {
     sub.pending=0;sub.generation=generation;
    } else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)ub_pc_unsubscribe(&sub);
   }
  }
  struct pollfd wait[3]={{fd,POLLIN,0},{event_fd,POLLIN,0},{sub.pending?sub.fd:-1,POLLOUT,0}};
  int ready=poll(wait,3,-1);
  if(ready<0){if(errno==EINTR)continue;break;}
  if(event_fd>=0&&(wait[1].revents&POLLIN))ub_pc_drain_event(event_fd);
  if(wait[0].revents&(POLLERR|POLLHUP|POLLNVAL))break;
  if(!(wait[0].revents&POLLIN))continue;
  ssize_t n=ub_pc_receive(fd,&request,sizeof(request),MSG_DONTWAIT,&from,&from_len,&peer);
  if(n<0){if(n==-1&&errno!=EINTR)break;continue;}
  if(!ub_pc_request_peer(&request,n,&peer)||from_len<=offsetof(struct sockaddr_un,sun_path))continue;
  if(from_len>sizeof(from))continue;
  if(UB_PC_NOW_MS(&begin))continue;
  if(begin-last<UB_PC_PERIOD_MS)usleep((unsigned int)(UB_PC_PERIOD_MS-(begin-last))*1000u);
  /* Bound work and keep only the latest queued request for this same client.
   * Other clients must retry; an old session cannot build an unbounded queue.
   */
  for(uint32_t i=0;i<UB_PC_DRAIN_LIMIT;i++) {
   n=ub_pc_receive(fd,&candidate,sizeof(candidate),MSG_DONTWAIT,&next,&next_len,&next_peer);
   if(n==-1)break;
   if(ub_pc_request_peer(&candidate,n,&next_peer)&&next_peer.pid==peer.pid&&
      next_len==from_len&&!memcmp(&next,&from,from_len))request=candidate;
  }
  if(request.op==UB_PC_STOP) {
   if(sub.fd>=0&&sub.pid==(uint32_t)peer.pid&&sub.length==from_len&&
      !memcmp(&sub.address,&from,from_len))ub_pc_unsubscribe(&sub);
   continue;
  }
  int subscribed=event_fd>=0&&paused&&ub_pc_subscribe(&sub,&from,from_len,(uint32_t)peer.pid,&request);
  if(UB_PC_NOW_MS(&begin))continue;
  last=begin;words[0]=words[1]=0;
  generation_before=0;paused_before=paused?paused(&generation_before):-1;
  int valid=get(words)==0;
  if(UB_PC_NOW_MS(&end))continue;
  if(end<begin||end-begin>UB_PC_MAX_AGE_MS)valid=0;
  generation=0;state=paused?paused(&generation):-1;
  ub_pc_pack(&reply,(uint32_t)getpid(),instance,++sequence,
   ub_pc_u64(request.client_nonce_lo,request.client_nonce_hi),request.request_sequence,
   begin,ub_pc_u64(words[0],words[1]),(uint32_t)valid);
  reply.native_generation=generation;
  if(subscribed&&valid&&state==1&&paused_before==1&&generation_before==generation)
   reply.flags|=UB_PC_PAUSED;
  if(subscribed){sub.generation=generation;sub.paused=(reply.flags&UB_PC_PAUSED)!=0;sub.pending=0;}
  (void)sendto(fd,&reply,sizeof(reply),MSG_DONTWAIT|MSG_NOSIGNAL,(struct sockaddr *)&from,from_len);
  }
  ub_pc_unsubscribe(&sub);close(fd);UB_PC_SOURCE_BACKOFF();
 }
}
struct ub_pc_client {
 int fd;uint32_t initialized,sequence,pending,peer_pid,paused,active,subscription_sequence;
 uint64_t nonce,request_ms,last_request_ms,retry_ms,peer_instance;
 struct ub_pc_state state;
};
static inline void ub_pc_client_init(struct ub_pc_client *c) {
 memset(c,0,sizeof(*c));c->fd=-1;c->initialized=1;
}
static inline void ub_pc_client_close(struct ub_pc_client *c) {
 if(c->initialized&&c->fd>=0)close(c->fd);
 ub_pc_client_init(c);
}
static inline void ub_pc_client_reset(struct ub_pc_client *c,uint64_t nonce) {
 uint64_t now=0;if(!c->initialized)ub_pc_client_init(c);
 (void)UB_PC_NOW_MS(&now);ub_pc_reset(&c->state,now);
 c->nonce=nonce;c->pending=0;c->paused=0;c->sequence++;c->last_request_ms=0;
}
static inline void ub_pc_client_wake(struct ub_pc_client *c) {
 if(!c->initialized)ub_pc_client_init(c);
 c->pending=0;c->paused=0;c->state.paused=0;c->sequence++;
 c->subscription_sequence=c->sequence;c->last_request_ms=0;
}
static inline void ub_pc_client_stop(struct ub_pc_client *c) {
 struct ub_pc_request request;struct sockaddr_un address;socklen_t length;
 if(c->fd<0||!c->nonce)return;
 ub_pc_request_pack(&request,c->nonce,++c->sequence);request.op=UB_PC_STOP;
 length=ub_pc_address(&address);
 if(length)(void)sendto(c->fd,&request,sizeof(request),MSG_DONTWAIT|MSG_NOSIGNAL,
  (struct sockaddr *)&address,length);
}
static inline void ub_pc_client_drain(struct ub_pc_client *c) {
 struct ub_pc_record discarded;struct ub_pc_credentials peer;
 if(c->fd<0)return;
 for(uint32_t i=0;i<UB_PC_DRAIN_LIMIT;i++)
  if(ub_pc_receive(c->fd,&discarded,sizeof(discarded),MSG_DONTWAIT,0,0,&peer)==-1)break;
}
/* One caller owns c. The call never waits for the source or native getter.
 * The caller changes nonce at renderer lifecycle/discontinuity boundaries and
 * checks that lifecycle again before publishing the returned position.
 */
static inline int ub_pc_client_step(struct ub_pc_client *c,uint64_t nonce,int active,
 uint32_t native_seconds,uint64_t *ticks) {
 struct ub_pc_record reply;struct ub_pc_credentials peer;uint64_t now,instance;
 if(!c->initialized)ub_pc_client_init(c);
 if(UB_PC_NOW_MS(&now)){c->state.valid=0;c->pending=0;c->paused=0;c->state.paused=0;ub_pc_client_drain(c);return 0;}
 if((!active||c->nonce!=nonce)&&c->active)ub_pc_client_stop(c);
 if(c->nonce!=nonce||!active)ub_pc_client_reset(c,nonce);
 c->active=active!=0;
 if(!active||!nonce) {
  /* Keep the autobound endpoint across pause/renderer lifecycle changes.
   * Nonce/sequence matching rejects old replies without a socket rebind delay. */
  c->peer_pid=0;c->peer_instance=0;ub_pc_client_drain(c);return 0;
 }
 if(c->fd<0) {
  struct sockaddr_un local;
  if(now<c->retry_ms)return 0;
  c->retry_ms=now+1000;
  c->fd=ub_pc_socket(1);if(c->fd<0)return 0;
  memset(&local,0,sizeof(local));local.sun_family=AF_UNIX;
  if(bind(c->fd,(struct sockaddr *)&local,offsetof(struct sockaddr_un,sun_path))) {
   close(c->fd);c->fd=-1;return 0;
  }
 }
 if(c->pending&&now-c->request_ms>UB_PC_MAX_AGE_MS)c->pending=0;
 for(uint32_t i=0;i<UB_PC_DRAIN_LIMIT;i++) {
  ssize_t n=ub_pc_receive(c->fd,&reply,sizeof(reply),MSG_DONTWAIT,0,0,&peer);
  if(n==-1)break;
  if(n!=(ssize_t)sizeof(reply)||peer.pid<=0||peer.uid!=(uint32_t)geteuid()||
     reply.pid!=(uint32_t)peer.pid)continue;
  instance=ub_pc_u64(reply.instance_lo,reply.instance_hi);
  if(reply.flags==UB_PC_EVENT) {
   uint32_t delta=reply.sequence-c->state.sequence;
   if(reply.magic==UB_PC_MAGIC&&reply.version==UB_PC_VERSION&&reply.bytes==sizeof(reply)&&
      c->peer_pid==(uint32_t)peer.pid&&c->peer_instance==instance&&
      ub_pc_u64(reply.client_nonce_lo,reply.client_nonce_hi)==c->nonce&&
      reply.request_sequence==c->subscription_sequence&&delta&&delta<0x80000000u&&
      reply.native_generation!=c->state.native_generation)ub_pc_client_wake(c);
   continue;
  }
  if(!c->pending)continue;
  if(c->peer_pid!=(uint32_t)peer.pid||c->peer_instance!=instance) {
   if(!ub_pc_peer_name((uint32_t)peer.pid,UB_PC_CLIENT_PEER))continue;
   c->peer_pid=(uint32_t)peer.pid;c->peer_instance=instance;
  }
  if(UB_PC_NOW_MS(&now)){c->state.valid=0;c->pending=0;c->paused=0;c->state.paused=0;ub_pc_client_drain(c);return 0;}
  if(now<c->request_ms||now-c->request_ms>UB_PC_MAX_AGE_MS)continue;
  int rc=ub_pc_accept(&c->state,&reply,(size_t)n,(uint32_t)peer.pid,peer.uid,
   c->peer_pid,(uint32_t)geteuid(),c->nonce,c->sequence,now,UB_PC_MAX_AGE_MS);
  if(rc==UB_PC_ACCEPTED){c->pending=0;c->paused=c->state.paused;}
 }
 if(!c->paused&&!c->pending&&now>=c->last_request_ms&&now-c->last_request_ms>=UB_PC_PERIOD_MS) {
  struct ub_pc_request request;struct sockaddr_un address;socklen_t len=ub_pc_address(&address);
  ub_pc_request_pack(&request,c->nonce,++c->sequence);c->last_request_ms=now;
  if(len&&sendto(c->fd,&request,sizeof(request),MSG_DONTWAIT|MSG_NOSIGNAL,
      (struct sockaddr *)&address,len)==(ssize_t)sizeof(request)) {
   c->pending=1;c->request_ms=now;c->state.barrier_ms=now;c->subscription_sequence=c->sequence;
  }
 }
 return ub_pc_sample(&c->state,now,UB_PC_MAX_AGE_MS,native_seconds,ticks);
}
#endif
