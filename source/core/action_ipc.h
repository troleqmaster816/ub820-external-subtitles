#ifndef UB_ACTION_IPC_H
#define UB_ACTION_IPC_H
/* Menu actions use one immutable request and reply per random client session
 * and sequence. Global status files describe current state, never acknowledge
 * an individual client. The legacy single mailbox remains readable for older
 * payloads, but new clients never publish to it. All files live in RAM. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#define UB_ACTION_PATH 192u
#define UB_ACTION_SESSION 32u

static int ub_action_path(char *out,size_t cap,const char *base,char kind,const char *session,unsigned seq){
 int n=snprintf(out,cap,"%s.%c.%s.%u",base,kind,session,seq);
 return n<0||(size_t)n>=cap?-1:0;
}
static int ub_action_write_expiring(const char *path,const char *text,size_t n,uint32_t expires){
 char temp[UB_ACTION_PATH];size_t done=0;ssize_t k;int fd,ok=1;
 int len=snprintf(temp,sizeof temp,"%s.tmp-%ld-%u",path,(long)getpid(),expires);
 if(len<0||(size_t)len>=sizeof temp)return -1;
 fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return -1;
 while(done<n){k=write(fd,text+done,n-done);if(k<0&&errno==EINTR)continue;if(k<=0){ok=0;break;}done+=(size_t)k;}
 if(close(fd))ok=0;
 if(ok&&!rename(temp,path))return 0;
 unlink(temp);return -1;
}
static inline int ub_action_write(const char *path,const char *text,size_t n){return ub_action_write_expiring(path,text,n,0);}
/* Expiry is CLOCK_MONOTONIC seconds, not wall-clock time. The envelope is
 * private to the transport: module parsers continue receiving their old text. */
static int ub_action_envelope(const char *path,const char *text,size_t n,uint32_t expires){
 char message[512];int len=snprintf(message,sizeof message,"UBACT1 %u\n",expires);
 if(len<0||(size_t)len>=sizeof message||n>sizeof message-(size_t)len)return -1;
 memcpy(message+len,text,n);return ub_action_write_expiring(path,message,(size_t)len+n,expires);
}
static char *ub_action_body(char *text,uint32_t *expires){
 uint32_t v=0;char *p=text+7;
 if(strncmp(text,"UBACT1 ",7)||*p<'0'||*p>'9')return 0;
 while(*p>='0'&&*p<='9'){
  unsigned d=(unsigned)(*p++-'0');if(v>429496729u||(v==429496729u&&d>5))return 0;
  v=v*10u+d;
 }
 if(*p++!='\n')return 0;
 *expires=v;return p;
}

#ifndef UB_PAYLOAD
#include "sys.h"
static uint32_t ub_action_clock(void){struct timespec t;ub_now(&t);return (uint32_t)t.tv_sec;}
static int ub_action_expired(const char *path,uint32_t now){
 char text[64];uint32_t expires;ssize_t n;const char *temp;int fd=open(path,O_RDONLY);
 if(fd<0)return 0;
 n=read(fd,text,sizeof text-1);close(fd);
 if(n>0){text[n]=0;if(ub_action_body(text,&expires))return (int32_t)(now-expires)>=0;}
 /* A process may die before writing even the envelope. Its temporary name
  * also carries expiry, so incomplete publications do not accumulate. */
 temp=strstr(path,".tmp-");
 if(temp){
  unsigned long pid;char tail;
  if(sscanf(temp,".tmp-%lu-%u%c",&pid,&expires,&tail)==2&&expires)return (int32_t)(now-expires)>=0;
 }
 return 0;
}
/* Strictly recognize only our final request names, not replies, temporary
 * publications or a request that another receiver has already claimed. */
static int ub_action_suffix(const char *p){
 unsigned i,n=0;
 if(strlen(p)<UB_ACTION_SESSION+2u)return 0;
 for(i=0;i<UB_ACTION_SESSION;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return 0;
 p+=UB_ACTION_SESSION;if(*p++!='.'||*p<'1'||*p>'9')return 0;
 while(*p>='0'&&*p<='9'){p++;if(++n>10)return 0;}
 return !*p;
}
static int ub_action_receive_deadline(const char *base,char *buf,size_t cap,char *reply,size_t replycap,uint32_t *deadline){
 char dir[UB_ACTION_PATH],path[UB_ACTION_PATH],claim[UB_ACTION_PATH];
 const char *name=strrchr(base,'/'),*entry;struct ub_dir d;size_t prefix;uint32_t now=ub_action_clock(),expires;int fd,n,found=0;ssize_t got,extra;
 if(!cap||!replycap)return 0;
 if(deadline)*deadline=0;
 reply[0]=0;
 if(!name)return 0;
 prefix=(size_t)(name-base);if(prefix>=sizeof dir)return 0;
 memcpy(dir,base,prefix);dir[prefix]=0;name++;
 if(!ub_dir_open(&d,dir)){
  while((entry=ub_dir_next(&d))){
   size_t len=strlen(name);
   int queued;
   if(strncmp(entry,name,len))continue;
   queued=!strncmp(entry+len,".q.",3);
   if(!queued&&strncmp(entry+len,".r.",3))continue;
   n=snprintf(path,sizeof path,"%s/%s",dir,entry);if(n<0||(size_t)n>=sizeof path)continue;
   /* Expired replies and abandoned claimed requests need no living client
    * to remove them. Never execute an expired request after its UI exited. */
   if(ub_action_expired(path,now)){unlink(path);continue;}
   if(!queued||!ub_action_suffix(entry+len+3))continue;
   n=snprintf(reply,replycap,"%s.r.%s",base,entry+len+3);if(n<0||(size_t)n>=replycap){reply[0]=0;continue;}
   found=1;break;
  }
  ub_dir_close(&d);
 }
 if(!found){if(strlen(base)>=sizeof path)return 0;strcpy(path,base);reply[0]=0;}
 n=snprintf(claim,sizeof claim,"%s.work",path);if(n<0||(size_t)n>=sizeof claim)return 0;
 /* Claim the pathname BEFORE opening it. A concurrent publisher's later
  * rename to the original path cannot be removed by our cleanup. */
 if(rename(path,claim))return 0;
 fd=open(claim,O_RDONLY);if(fd<0){unlink(claim);return 0;}
 do{got=read(fd,buf,cap-1);}while(got<0&&errno==EINTR);
 do{extra=read(fd,&n,1);}while(extra<0&&errno==EINTR);
 close(fd);unlink(claim);
 if(got<=0||extra!=0)return 0;
 buf[got]=0;
 if(found){
  char *body=ub_action_body(buf,&expires);
  if(!body||(int32_t)(now-expires)>=0)return 0;
  if(deadline)*deadline=expires;
  memmove(buf,body,strlen(body)+1);
 }
 return 1;
}
static int ub_action_receive(const char *base,char *buf,size_t cap,char *reply,size_t replycap){
 return ub_action_receive_deadline(base,buf,cap,reply,replycap,0);
}
static int ub_action_reply(const char *reply,const char *text,size_t n){
 return reply[0]?ub_action_envelope(reply,text,n,ub_action_clock()+60u):0;
}
#endif
#endif
