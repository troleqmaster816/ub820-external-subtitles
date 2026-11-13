/* Explicit opt-in CPU wait optimization. Kernel calls live in a short-lived
 * worker, never in the helper's event loop. Resident code is never unloaded;
 * an uncertain activation is latched until an explicit, verified Off/reset. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include "ub.h"
#include "tools.h"
#include "helper_menu.h"
#include "action_ipc.h"
#include "storage.h"
#include "control.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern const unsigned char ub_tool_cpu_wait[];
extern const size_t ub_tool_cpu_wait_size;
#define ID "cpu"
#define OWNER UB_CPU_DIR "/owner.bin"
#define RECEIPT UB_CPU_DIR "/instance.bin"
#define LATCH UB_CPU_DIR "/pending.bin"
#define LOCK UB_CPU_DIR "/worker.lock"
#define CPU_RESIDENT_BYTES 74345u
#define CPU_STOCK_WORD UINT32_C(0xb4ffff53)
static const uint64_t sites[2]={UINT64_C(0xffffffc000626ab0),UINT64_C(0xffffffc00062aa60)};
static int optimize,effective=-1,started,automatic_pending,automatic_blocked,cleanup_pending;
static unsigned menu_seq;
static char message[160]="not queried";
static struct cpu_owner owner;
static struct {
 pid_t pid;int active,timed_out,operation,manual,expected,deferred_off;uint64_t deadline;
 unsigned seq;unsigned char nonce[16];char reply[UB_ACTION_PATH],job[192],result[192];
} pending;

static int exact_read(int fd,void *out,size_t n){
 size_t used=0;char extra;ssize_t k;
 while(used<n){k=read(fd,(char *)out+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0)return -1;used+=(size_t)k;}
 do{k=read(fd,&extra,1);}while(k<0&&errno==EINTR);return k==0?0:-1;
}
static int private_fd(int fd,int regular){
 struct ub_stat s;return ub_fstat(fd,&s)||s.uid!=(unsigned)geteuid()||
  (s.mode&0777)!=0600||(regular&&!S_ISREG(s.mode))?-1:0;
}
static int read_record(const char *path,void *out,size_t n){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK),rc;if(fd<0)return -1;
 rc=private_fd(fd,1)||exact_read(fd,out,n);close(fd);return rc?-1:0;
}
static int random_bytes(void *out,size_t n){
 int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);size_t used=0;ssize_t k;if(fd<0)return -1;
 while(used<n){k=read(fd,(char *)out+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0){close(fd);return -1;}used+=(size_t)k;}
 close(fd);return 0;
}
static void hex_nonce(const unsigned char *raw,char text[33]){
 static const char h[]="0123456789abcdef";for(unsigned i=0;i<16;i++){text[i*2]=h[raw[i]>>4];text[i*2+1]=h[raw[i]&15];}text[32]=0;
}
static int parse_nonce(const char *s,unsigned char raw[16]){
 if(strlen(s)!=32)return -1;
 for(unsigned i=0;i<16;i++){unsigned v=0;for(unsigned j=0;j<2;j++){unsigned char c=(unsigned char)s[2*i+j];if(c>='0'&&c<='9')v=v*16+c-'0';else if(c>='a'&&c<='f')v=v*16+c-'a'+10;else return -1;}raw[i]=(unsigned char)v;}return 0;
}
static int atomic_file(const char *path,const void *data,size_t n,int replace){
 char temp[256],suffix[33];unsigned char nonce[16];size_t used=0;int fd,rc=0;ssize_t k;
 if(random_bytes(nonce,sizeof nonce))return -1;
 hex_nonce(nonce,suffix);
 if(snprintf(temp,sizeof temp,"%s.%s.tmp",path,suffix)>=(int)sizeof temp)return -1;
 fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 while(used<n){k=write(fd,(const char *)data+used,n-used);if(k<0&&errno==EINTR)continue;if(k<=0){rc=-1;break;}used+=(size_t)k;}
 if(close(fd))rc=-1;
 if(!rc){if(replace){if(rename(temp,path))rc=-1;}else if(link(temp,path))rc=-1;}
 unlink(temp);return rc;
}
static int runtime_dir(void){
 struct statfs fs;int fd;struct ub_stat s;
 if(statfs("/tmp",&fs)||(unsigned long)fs.f_type!=0x01021994ul)return -1;
 if(mkdir(UB_CPU_DIR,0700)&&errno!=EEXIST)return -1;
 fd=open(UB_CPU_DIR,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 int rc=ub_fstat(fd,&s)||!S_ISDIR(s.mode)||s.uid!=(unsigned)geteuid()||(s.mode&0777)!=0700;close(fd);return rc?-1:0;
}
static int boot_id(char boot[40]){
 char text[64];ssize_t n=ub_read_file("/proc/sys/kernel/random/boot_id",text,sizeof text);
 if(n!=37||text[36]!='\n')return -1;
 for(unsigned i=0;i<36;i++){int dash=i==8||i==13||i==18||i==23;if(dash?text[i]!='-':!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return -1;}
 memset(boot,0,40);memcpy(boot,text,36);return 0;
}
static uint64_t birth(unsigned pid){
 char path[64],text[2048],*p,*end;unsigned field;unsigned long long v;
 snprintf(path,sizeof path,"/proc/%u/stat",pid);if(ub_read_file(path,text,sizeof text)<=0)return 0;
 p=strrchr(text,')');if(!p||p[1]!=' ')return 0;p+=2;
 for(field=3;field<22;field++){p=strchr(p,' ');if(!p)return 0;while(*p==' ')p++;}
 if(*p<'0'||*p>'9')return 0;
 errno=0;v=strtoull(p,&end,10);return errno||end==p||(*end!=' '&&*end!='\n'&&*end)?0:(uint64_t)v;
}
static int owner_valid(const struct cpu_owner *o,const char *boot){
 return o->magic==UB_CPU_MAGIC&&o->version==UB_CPU_VERSION&&o->enabled<=1&&o->desired<=1&&
  o->reserved==0&&o->pid>1&&o->born&&birth(o->pid)==o->born&&!memcmp(o->boot,boot,40);
}
static int current_owner(struct cpu_owner *o,const char *boot){return read_record(OWNER,o,sizeof *o)||!owner_valid(o,boot)?-1:0;}
static int publish_owner(int enabled,int desired){
 owner.enabled=(unsigned)enabled;owner.desired=(unsigned)desired;return atomic_file(OWNER,&owner,sizeof owner,1);
}
static int trailing_space(const char *p){for(;*p;p++)if(*p!=' '&&*p!='\n'&&*p!='\r'&&*p!='\t')return 0;return 1;}
static int literal(const char **p,const char *word){size_t n=strlen(word);if(strncmp(*p,word,n))return -1;*p+=n;return 0;}
static int unsigned_number(const char **p,uint64_t max,uint64_t *out){
 uint64_t v=0;const char *s=*p;if(*s<'0'||*s>'9')return -1;
 do{unsigned d=(unsigned)(*s++-'0');if(v>max/10u||(v==max/10u&&d>max%10u))return -1;v=v*10u+d;}while(*s>='0'&&*s<='9');
 *p=s;*out=v;return 0;
}
static int u32_field(const char **p,const char *key,unsigned *out){uint64_t v;if(literal(p,key)||unsigned_number(p,UINT_MAX,&v))return -1;*out=(unsigned)v;return 0;}
static int i32_field(const char **p,const char *key,int *out){
 uint64_t v;int negative;if(literal(p,key))return -1;negative=**p=='-';if(negative)(*p)++;
 if(unsigned_number(p,(uint64_t)INT_MAX+(unsigned)negative,&v))return -1;
 *out=negative?(v==(uint64_t)INT_MAX+1?INT_MIN:-(int)v):(int)v;return 0;
}
static int observed_field(const char **p){
 uint64_t v;int negative;if(literal(p," observed_task_state="))return -1;negative=**p=='-';if(negative)(*p)++;
 return unsigned_number(p,(uint64_t)INT64_MAX+(unsigned)negative,&v);
}
static int native_parse(const char *text,struct cpu_native *s){
 const char *p=text;memset(s,0,sizeof *s);
 if(literal(&p,"cpu_wait=1 build="))return -1;
 if(!strncmp(p,"1.82 ",5))memcpy(s->build,"1.82",5);else if(!strncmp(p,"1.85 ",5))memcpy(s->build,"1.85",5);else return -1;p+=4;
 if(u32_field(&p," phase=",&s->phase)||u32_field(&p," pinned_until_reset=",&s->pinned)||u32_field(&p," intended_enabled=",&s->enabled)||
    i32_field(&p," rc=",&s->rc)||i32_field(&p," fault=",&s->fault)||i32_field(&p," begin_rc=",&s->begin_rc))return -1;
 for(unsigned i=0;i<2;i++)if(u32_field(&p,i?"\nantena pid=":"\nfpd pid=",&s->pid[i])||u32_field(&p," bypass=",&s->bypass[i])||u32_field(&p," rejected=",&s->rejected[i])||observed_field(&p))return -1;
 return trailing_space(p)?0:-1;
}
static int native_status(struct cpu_native *s){
 char text[1024];int fd=open(UB_CPU_NODE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);size_t used=0;ssize_t n;struct ub_stat st;
 if(fd<0)return -1;
 if(ub_fstat(fd,&st)||!S_ISCHR(st.mode)||st.uid||((st.mode&0777)!=0600)){close(fd);return -1;}
 while(used<sizeof text-1){n=read(fd,text+used,sizeof text-1-used);if(n<0&&errno==EINTR)continue;if(n<0){close(fd);return -1;}if(!n)break;used+=(size_t)n;}
 char extra;if(used==sizeof text-1){do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);if(n){close(fd);return -1;}}
 close(fd);text[used]=0;return !used||memchr(text,0,used)?-1:native_parse(text,s);
}
/* 0 absent; 1 exactly our module; -1 any inconsistent or foreign CPU entry. */
static int instance(struct cpu_instance *out){
 char text[16384],*line,*next;ssize_t n=ub_read_file(UB_CPU_MODULES,text,sizeof text);int found=0;
 if(n<0||(size_t)n>=sizeof text-1)return -1;
 memset(out,0,sizeof *out);
 for(line=text;*line;line=next){char name[80],deps[256],state[16],address[32];unsigned size,refs;char *end;unsigned long long base;int used=0;
  next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);
  if(strncmp(line,"ub820_cpu_",10))continue;
  if(sscanf(line,"%79s %u %u %255s %15s %31s%n",name,&size,&refs,deps,state,address,&used)!=6||strcmp(name,UB_CPU_MODULE_NAME)||found++||strcmp(state,"Live"))return -1;
  errno=0;base=strtoull(address,&end,16);if(errno||*end||!base||size!=CPU_RESIDENT_BYTES||base>UINT64_MAX-size)return -1;
  out->base=base;out->size=size;out->references=refs;
 }
 return found?1:0;
}
static uint16_t le16(const unsigned char *p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const unsigned char *p){return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static int pread_exact(int fd,void *data,size_t n,uint64_t off){
 if(off>INT64_MAX||n>(uint64_t)INT64_MAX-off)return -1;
 size_t done=0;while(done<n){ssize_t k=pread64(fd,(char *)data+done,n-done,(off64_t)(off+done));if(k<0&&errno==EINTR)continue;if(k<=0)return -1;done+=(size_t)k;}return 0;
}
static int code_words(int active,const struct cpu_instance *m){
 unsigned char h[64],ph[32*56],raw[4];uint64_t offsets[2]={0,0};unsigned matches[2]={0,0};
 int fd=open("/proc/kcore",O_RDONLY|O_CLOEXEC|O_NOFOLLOW),rc=-1;unsigned count;
 if(fd<0)return -1;
 if(pread_exact(fd,h,sizeof h,0)||memcmp(h,"\177ELF\2\1\1",7)||le16(h+16)!=4||le16(h+18)!=183||le16(h+52)!=64||le16(h+54)!=56)goto out;
 count=le16(h+56);if(!count||count>32||pread_exact(fd,ph,count*56u,le64(h+32)))goto out;
 for(unsigned i=0;i<count;i++){unsigned char *p=ph+i*56;uint64_t at=le64(p+16),size=le64(p+32),off=le64(p+8);if(le32(p)!=1)continue;
  for(unsigned j=0;j<2;j++)if(sites[j]>=at&&sites[j]-at<=size&&size-(sites[j]-at)>=4){uint64_t delta=sites[j]-at;if(off>UINT64_MAX-delta)goto out;offsets[j]=off+delta;matches[j]++;}}
 for(unsigned j=0;j<2;j++){uint32_t word;if(matches[j]!=1||pread_exact(fd,raw,4,offsets[j]))goto out;word=le32(raw);
  if(!active){if(word!=CPU_STOCK_WORD)goto out;}
  else{int64_t delta=(int64_t)(int32_t)((word&0x03ffffffu)<<6)>>4;uint64_t target=sites[j]+delta;if((word&0xfc000000u)!=0x14000000u||target<m->base||target-m->base>=m->size)goto out;}}
 rc=0;
out:close(fd);return rc;
}
static int trusted(const struct cpu_receipt *r,const struct cpu_instance *m,const struct cpu_native *s,const char *boot){
 if(r->magic!=UB_CPU_MAGIC||r->version!=UB_CPU_VERSION||r->base!=m->base||r->size!=m->size||memcmp(r->boot,boot,40)||memcmp(r->sha,UB_CPU_ARTIFACT_SHA256,65))return 0;
 for(unsigned i=0;i<2;i++)if(r->pid[i]!=s->pid[i]||!r->born[i]||birth(r->pid[i])!=r->born[i])return 0;
 int mode=cpu_native_mode(s);if(mode<0||(!s->pinned&&m->references)|| (s->pinned&&m->references!=1))return 0;
 return !code_words(mode,m);
}
static int observe(const char *boot,struct cpu_instance *m,struct cpu_native *s,struct cpu_receipt *r){
 struct ub_stat st;int exists=instance(m);if(exists<0)return -1;
 int fd=open(UB_CPU_NODE,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 if(!exists){if(fd>=0){close(fd);return -1;}if(errno!=ENOENT)return -1;return code_words(0,m)?-1:0;}
 if(fd<0)return -1;
 int bad=ub_fstat(fd,&st)||!S_ISCHR(st.mode)||st.uid||(st.mode&0777)!=0600;close(fd);
 if(bad||read_record(RECEIPT,r,sizeof *r)||native_status(s)||!trusted(r,m,s,boot))return -1;
 return 1;
}
enum { LATCH_INVALID=-1,LATCH_NONE=0,LATCH_ON=1,LATCH_OFF=2 };
static int latch_state(const char *boot){
 struct cpu_latch l;int fd=open(LATCH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?LATCH_NONE:LATCH_INVALID;
 int bad=private_fd(fd,1)||exact_read(fd,&l,sizeof l);close(fd);
 if(bad||l.magic!=UB_CPU_MAGIC||l.version!=UB_CPU_VERSION||l.pending!=1||(l.operation!=CPU_ON&&l.operation!=CPU_OFF))return LATCH_INVALID;
 for(unsigned i=0;i<40;i++){
  unsigned char c=(unsigned char)l.boot[i];int dash=i==8||i==13||i==18||i==23;
  if(i>=36?c!=0:dash?c!='-':!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return LATCH_INVALID;
 }
 if(memcmp(l.boot,boot,40))return LATCH_NONE;
 return l.operation==CPU_ON?LATCH_ON:LATCH_OFF;
}
static int latch_pending(const char *boot){return latch_state(boot)!=LATCH_NONE;}
static int set_latch(const struct cpu_job *job,unsigned operation){
 struct cpu_latch l;memset(&l,0,sizeof l);l.magic=UB_CPU_MAGIC;l.version=UB_CPU_VERSION;l.pending=1;l.operation=operation;memcpy(l.boot,job->owner.boot,40);memcpy(l.nonce,job->nonce,16);return atomic_file(LATCH,&l,sizeof l,1);
}
static int native_write(int on,uint32_t *off_attempted){
 const char *command=on?"ON\n":"OFF\n";size_t n=on?3u:4u;
 int fd=open(UB_CPU_NODE,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);struct ub_stat st;if(fd<0)return -1;
 if(ub_fstat(fd,&st)||!S_ISCHR(st.mode)||st.uid||(st.mode&0777)!=0600){close(fd);return -1;}
 /* One write, including EINTR. Never complete a partial native command. */
 if(!on)*off_attempted=1;
 ssize_t wrote=write(fd,command,n);int closed=close(fd);return wrote==(ssize_t)n&&!closed?0:-1;
}
static int load_inert(const char *boot,struct cpu_instance *m,struct cpu_native *s,struct cpu_receipt *r){
 char hash[65];struct utsname u;
 if(uname(&u)||strcmp(u.release,"4.4.8")||(strcmp(u.machine,"aarch64")&&strcmp(u.machine,"armv8l"))||ub_tool_cpu_wait_size!=UB_CPU_ARTIFACT_BYTES)return -1;
 ub_hash_bytes(ub_tool_cpu_wait,ub_tool_cpu_wait_size,hash);if(strcmp(hash,UB_CPU_ARTIFACT_SHA256))return -1;
 if(syscall(SYS_init_module,ub_tool_cpu_wait,ub_tool_cpu_wait_size,"")||instance(m)!=1||native_status(s)||s->phase!=0||cpu_native_mode(s)!=0||m->references||code_words(0,m))return -1;
 memset(r,0,sizeof *r);r->magic=UB_CPU_MAGIC;r->version=UB_CPU_VERSION;r->size=m->size;r->base=m->base;r->preference=-1;
 memcpy(r->boot,boot,40);memcpy(r->sha,UB_CPU_ARTIFACT_SHA256,65);
 for(unsigned i=0;i<2;i++){r->pid[i]=s->pid[i];r->born[i]=birth(r->pid[i]);if(!r->born[i])return -1;}
 return atomic_file(RECEIPT,r,sizeof *r,1);
}
static void result_set(struct cpu_result *r,int mode,int code,const char *why){r->mode=mode;r->code=code;snprintf(r->message,sizeof r->message,"%s",why);}
static int worker(const char *nonce){
 char jobpath[192],resultpath[192],boot[40];struct cpu_job j;struct cpu_result out;struct cpu_owner latest;
 struct cpu_instance m;struct cpu_native s;struct cpu_receipt receipt;unsigned char raw[16];int lock=-1,state=-1,wrote=0;
 if(parse_nonce(nonce,raw)||runtime_dir()||boot_id(boot))return 2;
 snprintf(jobpath,sizeof jobpath,UB_CPU_DIR "/job.%s",nonce);snprintf(resultpath,sizeof resultpath,UB_CPU_DIR "/result.%s",nonce);
 if(read_record(jobpath,&j,sizeof j)||j.magic!=UB_CPU_MAGIC||j.version!=UB_CPU_VERSION||j.operation>CPU_RECONCILE||j.manual>1||
    (j.operation==CPU_RECONCILE&&j.manual)||memcmp(j.nonce,raw,16)||!owner_valid(&j.owner,boot))return 2;
 memset(&out,0,sizeof out);out.magic=UB_CPU_MAGIC;out.version=UB_CPU_VERSION;out.preference=-1;memcpy(out.nonce,raw,16);result_set(&out,-1,CPU_INVALID,"identity or native state unavailable");
 lock=open(LOCK,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(lock<0||private_fd(lock,1))goto done;
 /* Only lifecycle Off may wait for an older lease. The parent remains
  * asynchronous and never kills/retries this waiter. No load/On is reachable
  * from it, and its policy is checked again after the kernel wakes flock. */
 int cleanup=j.operation==CPU_RECONCILE;
 if(flock(lock,LOCK_EX|(cleanup?0:LOCK_NB))){result_set(&out,-1,CPU_BUSY,"a previous CPU operation still owns the device");goto done;}
 if(current_owner(&latest,boot)||(!cleanup&&(!cpu_owner_equal(&latest,&j.owner)||ub_now_ms()>=j.deadline))){result_set(&out,-1,CPU_STALE,"expired or superseded CPU request");goto done;}
 int latched=latch_state(boot);out.suppress_auto_on=latched!=LATCH_NONE;
 state=observe(boot,&m,&s,&receipt);if(state<0){result_set(&out,-1,CPU_INVALID,"native identity/state unavailable; restart player required");goto done;}
 int mode=state?cpu_native_mode(&s):0;
 if(cleanup&&latest.enabled&&latest.desired){result_set(&out,mode,CPU_OK,"Off cleanup superseded by the latest On policy");goto done;}
 /* A previous uncertain Off is not a new authorization to write Off again.
  * A healthy stock-Off observation below may still resolve it without I/O. */
 if(cleanup&&mode==1&&(latched==LATCH_OFF||latched==LATCH_INVALID)){
  result_set(&out,-1,CPU_UNCERTAIN,"previous Off or transaction record is unresolved; select Off explicitly or restart player");goto done;
 }
 if(j.operation==CPU_QUERY){result_set(&out,mode,CPU_OK,out.suppress_auto_on?"previous activation is unresolved; select Off to recover":"ready");goto done;}
 if(j.operation==CPU_ON){
  if(!cpu_on_allowed(&latest,&j.owner,out.suppress_auto_on)){result_set(&out,-1,CPU_UNCERTAIN,"activation is blocked; select Off to verify recovery");goto done;}
  if(mode==1){result_set(&out,1,CPU_OK,"already on");goto reconcile;}
  if(set_latch(&j,CPU_ON))goto done;
  out.suppress_auto_on=1;
  if(current_owner(&latest,boot)||!cpu_on_allowed(&latest,&j.owner,0)||ub_now_ms()>=j.deadline){result_set(&out,-1,CPU_STALE,"activation cancelled before kernel load; select Off to recover");goto done;}
  if(!state&&load_inert(boot,&m,&s,&receipt))goto done;
  if(current_owner(&latest,boot)||!cpu_on_allowed(&latest,&j.owner,0)||ub_now_ms()>=j.deadline){result_set(&out,-1,CPU_STALE,"activation cancelled before native On; select Off to recover");goto done;}
  wrote=native_write(1,&out.off_attempted);
 }else{
  if(!state){if(unlink(LATCH)&&errno!=ENOENT)goto done;out.suppress_auto_on=0;result_set(&out,0,CPU_OK,"off; kernel component not loaded");goto done;}
  if(mode==0){if(unlink(LATCH)&&errno!=ENOENT)goto done;out.suppress_auto_on=0;result_set(&out,0,CPU_OK,"already off");goto done;}
  /* An old Off may not override a newer generation that explicitly wants On. */
  if(current_owner(&latest,boot)||((cleanup||!cpu_owner_equal(&latest,&j.owner))&&latest.enabled&&latest.desired)||(!cleanup&&ub_now_ms()>=j.deadline)){result_set(&out,-1,CPU_STALE,"superseded or expired Off request");goto done;}
  if(set_latch(&j,CPU_OFF))goto done;
  out.suppress_auto_on=1;
  wrote=native_write(0,&out.off_attempted);
 }
 state=observe(boot,&m,&s,&receipt);mode=state==1?cpu_native_mode(&s):-1;
 if(wrote||mode!=(j.operation==CPU_ON)){result_set(&out,-1,CPU_UNCERTAIN,state<0?"native state unhealthy or unverified; restart player required":"native change was not fully acknowledged; no automatic resend");goto reconcile;}
 if(unlink(LATCH)&&errno!=ENOENT){result_set(&out,-1,CPU_UNCERTAIN,"cannot clear activation receipt");goto reconcile;}
 out.suppress_auto_on=0;result_set(&out,mode,CPU_OK,"applied");
reconcile:
 /* Completion hand-off: a new helper can publish Off while this worker was
  * blocked in the native call. The same lock owner performs the correction.
  * A GET never enters this path. No polling or second activation occurs. */
 if(j.operation==CPU_ON&&!current_owner(&latest,boot)&&state==1&&cpu_completion_needs_off(&latest,cpu_native_mode(&s))){
  if(set_latch(&j,CPU_OFF)){out.suppress_auto_on=1;result_set(&out,-1,CPU_UNCERTAIN,"cannot record latest Off intent");goto done;}
  wrote=native_write(0,&out.off_attempted);state=observe(boot,&m,&s,&receipt);
  if(!wrote&&state==1&&cpu_native_mode(&s)==0){if(!unlink(LATCH)||errno==ENOENT){out.suppress_auto_on=0;result_set(&out,0,CPU_OK,"off for the latest helper policy");}}
  else{out.suppress_auto_on=1;result_set(&out,-1,CPU_UNCERTAIN,"latest Off policy was not confirmed");}
 }
done:
 if(out.code!=CPU_OK&&j.operation==CPU_ON)out.suppress_auto_on=1;
 /* Keep the lock until the final atomic acknowledgement exists. */
 int rc=atomic_file(resultpath,&out,sizeof out,0);if(lock>=0)close(lock);return rc?3:0;
}
static void clean_worker(const char *nonce){
 struct ub_dir d;const char *e;sigset_t empty;int nul;
 for(int sig=1;sig<65;sig++)signal(sig,SIG_DFL);
 sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,0);umask(077);
 if(!ub_dir_open(&d,"/proc/self/fd")){while((e=ub_dir_next(&d)))if(e[0]>='0'&&e[0]<='9'){int fd=atoi(e);if(fd>2&&fd!=d.fd)close(fd);}ub_dir_close(&d);}
 else{struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim)||lim.rlim_max==RLIM_INFINITY||lim.rlim_max>1048576)_exit(126);for(rlim_t fd=3;fd<lim.rlim_max;fd++)close((int)fd);}
 nul=open("/dev/null",O_RDWR|O_CLOEXEC);if(nul<0||dup2(nul,0)<0||dup2(nul,1)<0||dup2(nul,2)<0)_exit(126);if(nul>2)close(nul);
 execl("/proc/self/exe","engage","--cpu-worker",nonce,(char *)0);_exit(127);
}
static int module_enabled;
static const char *mode_name(int mode){return mode<0?"unknown":mode?"on":"off";}
static void publish_status(void){
 if(!module_enabled){unlink(UB_CPU_ENABLED);unlink(UB_CPU_STATUS);return;}
 char text[256];int n=snprintf(text,sizeof text,"%u %s %s\n",menu_seq,mode_name(effective),message);
 if(n>0&&(size_t)n<sizeof text&&!atomic_file(UB_CPU_STATUS,text,(size_t)n,1))atomic_file(UB_CPU_ENABLED,"1\n",2,1);
}
static void answer(const char *reply,unsigned seq){
 char text[256];int n;if(!reply||!*reply)return;
 n=snprintf(text,sizeof text,"%u %s %s\n",seq,mode_name(effective),message);if(n>0&&(size_t)n<sizeof text)ub_action_reply(reply,text,(size_t)n);
}
static int begin(int operation,int manual,unsigned seq,const char *reply,uint32_t expires){
 struct cpu_job j;char nonce[33];pid_t pid;if(pending.active)return -1;
 memset(&j,0,sizeof j);j.magic=UB_CPU_MAGIC;j.version=UB_CPU_VERSION;j.operation=(unsigned)operation;j.manual=(unsigned)manual;j.owner=owner;
 uint64_t now=ub_now_ms();j.deadline=now+15000;
 if(expires){uint64_t d=(uint64_t)expires*1000;if(d<=now)return -1;if(d<j.deadline)j.deadline=d;}
 if(random_bytes(j.nonce,16))return -1;
 hex_nonce(j.nonce,nonce);
 memset(&pending,0,sizeof pending);snprintf(pending.job,sizeof pending.job,UB_CPU_DIR "/job.%s",nonce);snprintf(pending.result,sizeof pending.result,UB_CPU_DIR "/result.%s",nonce);
 if(atomic_file(pending.job,&j,sizeof j,0))return -1;
 pending.active=1;pending.operation=operation;pending.manual=manual;pending.expected=operation==CPU_ON;pending.seq=seq;pending.deadline=j.deadline;memcpy(pending.nonce,j.nonce,16);if(reply)snprintf(pending.reply,sizeof pending.reply,"%s",reply);
 pid=fork();if(!pid)clean_worker(nonce);if(pid<0){unlink(pending.job);memset(&pending,0,sizeof pending);return -1;}pending.pid=pid;return 0;
}
static void poll_worker(void){
 int status;pid_t rc;struct cpu_result result;
 if(!pending.active)return;
 do{rc=waitpid(pending.pid,&status,WNOHANG);}while(rc<0&&errno==EINTR);
 if(!rc){if(!pending.timed_out&&ub_now_ms()>=pending.deadline){pending.timed_out=1;automatic_pending=0;automatic_blocked=1;effective=-1;snprintf(message,sizeof message,"CPU operation has not returned; no automatic retry");ub_log(ID,UB_ALWAYS,"%s; worker retained for completion",message);answer(pending.reply,pending.seq);pending.reply[0]=0;publish_status();}return;}
 if(ub_now_ms()>=pending.deadline){pending.timed_out=1;automatic_pending=0;automatic_blocked=1;}
 int valid=rc==pending.pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&!read_record(pending.result,&result,sizeof result)&&result.magic==UB_CPU_MAGIC&&result.version==UB_CPU_VERSION&&!memcmp(result.nonce,pending.nonce,16)&&result.mode>=-1&&result.mode<=1&&result.off_attempted<=1&&memchr(result.message,0,sizeof result.message);
 int operation=pending.operation,manual=pending.manual;
 /* A manual Off arriving during an older operation gets one follow-up only
  * when a valid result proves that older worker never attempted Off. An
  * uncertain attempted Off, or a lost result, cannot authorize a resend. */
 if(pending.deferred_off&&valid&&!result.off_attempted&&!(result.code==CPU_OK&&result.mode==0)&&ub_now_ms()<pending.deadline){
  char reply[UB_ACTION_PATH];unsigned seq=pending.seq;uint32_t expiry=(uint32_t)(pending.deadline/1000);
  snprintf(reply,sizeof reply,"%s",pending.reply);unlink(pending.job);unlink(pending.result);memset(&pending,0,sizeof pending);
  if(!begin(CPU_OFF,1,seq,reply,expiry))return;
  effective=-1;snprintf(message,sizeof message,"Off could not be queued after the previous operation");answer(reply,seq);publish_status();return;
 }
 effective=valid&&result.code==CPU_OK?result.mode:-1;menu_seq=pending.seq;
 if(!valid||result.suppress_auto_on)automatic_blocked=1;
 snprintf(message,sizeof message,"%s",valid?result.message:"CPU worker ended without a verified result");
 if(manual&&effective==pending.expected&&!pending.timed_out){int saved=ub_setting_set(ID,"optimize",mode_name(effective));if(saved)snprintf(message,sizeof message,"applied; not saved to USB (%s)",ub_stick_error(saved));else snprintf(message,sizeof message,"saved");}
 ub_log(ID,effective<0?UB_ALWAYS:UB_INFO,"effective %s: %s",mode_name(effective),message);
 answer(pending.reply,pending.seq);unlink(pending.job);unlink(pending.result);memset(&pending,0,sizeof pending);publish_status();
 if(!module_enabled){if(cleanup_pending){cleanup_pending=0;if(!(valid&&result.off_attempted&&effective<0))begin(CPU_RECONCILE,0,0,0,0);}return;}
 if(automatic_pending&&(operation==CPU_QUERY||operation==CPU_RECONCILE)){automatic_pending=0;
  if(effective>=0&&effective!=optimize&&(!optimize||!automatic_blocked)){if(publish_owner(1,optimize)||begin(optimize?CPU_ON:CPU_OFF,0,0,0,0)){effective=-1;snprintf(message,sizeof message,"could not queue saved CPU preference");publish_status();}}}
}
static int parse_request(const char *text,int *operation,unsigned *seq){
 const char *p;uint64_t v;
 if(!strncmp(text,"get ",4)){*operation=CPU_QUERY;p=text+4;}
 else if(!strncmp(text,"set on ",7)){*operation=CPU_ON;p=text+7;}
 else if(!strncmp(text,"set off ",8)){*operation=CPU_OFF;p=text+8;}
 else return -1;
 if(*p<'1'||*p>'9'||unsigned_number(&p,UINT_MAX,&v)||(*p&&strcmp(p,"\n")))return -1;
 *seq=(unsigned)v;return 0;
}
static void handle_request(int op,unsigned seq,const char *reply,uint32_t expiry){
 if((int32_t)((uint32_t)(ub_now_ms()/1000)-expiry)>=0)return;
 if(pending.active){
  if(op==CPU_OFF){if(!publish_owner(1,0)){
   automatic_pending=0;effective=-1;snprintf(message,sizeof message,"superseded by a newer Off request");answer(pending.reply,pending.seq);
   pending.expected=0;pending.manual=1;pending.deferred_off=1;pending.seq=seq;snprintf(pending.reply,sizeof pending.reply,"%s",reply);
   uint64_t request_deadline=(uint64_t)expiry*1000;pending.deadline=ub_now_ms()+15000;if(request_deadline<pending.deadline)pending.deadline=request_deadline;pending.timed_out=0;
   snprintf(message,sizeof message,"Off intent queued; previous native operation is still running");return;
  }else{effective=-1;snprintf(message,sizeof message,"cannot publish Off intent");}}
  else{effective=-1;snprintf(message,sizeof message,"a previous CPU operation is still running");}
  answer(reply,seq);return;
 }
 automatic_pending=0;
 if(op!=CPU_QUERY&&publish_owner(1,op==CPU_ON)){effective=-1;snprintf(message,sizeof message,"cannot publish CPU policy");answer(reply,seq);return;}
 if(begin(op,op!=CPU_QUERY,seq,reply,expiry)){effective=-1;snprintf(message,sizeof message,"could not start CPU worker");answer(reply,seq);publish_status();}
}
static void menu_request(void){
 char text[80],reply[UB_ACTION_PATH];uint32_t expiry;unsigned seq;int op;
 if(!ub_action_receive_deadline(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply,&expiry)||!reply[0]||!expiry)return;
 if(parse_request(text,&op,&seq))return;
 const char *suffix=strrchr(reply,'.');char expected[12];snprintf(expected,sizeof expected,"%u",seq);if(!suffix||strcmp(suffix+1,expected))return;
 handle_request(op,seq,reply,expiry);
}
static void initialize(int enabled){
 started=1;module_enabled=enabled;effective=-1;automatic_pending=enabled&&optimize;cleanup_pending=0;unlink(UB_CPU_ENABLED);unlink(UB_CPU_STATUS);
 memset(&owner,0,sizeof owner);owner.magic=UB_CPU_MAGIC;owner.version=UB_CPU_VERSION;owner.pid=(unsigned)getpid();owner.born=birth(owner.pid);
 if(runtime_dir()||boot_id(owner.boot)||!owner.born||random_bytes(owner.token,16)||publish_owner(enabled,enabled?optimize:0)||begin(enabled&&optimize?CPU_QUERY:CPU_RECONCILE,0,0,0,0)){automatic_pending=0;automatic_blocked=1;snprintf(message,sizeof message,"CPU control is unavailable");}
 publish_status();
}
static void start(struct ub_module *m){initialize(m->enabled);}
static void step(struct ub_part *p,struct ub_target *t){
 (void)p;(void)t;
 if(!module_enabled){module_enabled=1;automatic_pending=optimize;cleanup_pending=0;if(publish_owner(1,optimize)||(!pending.active&&begin(optimize?CPU_QUERY:CPU_RECONCILE,0,0,0,0))){automatic_pending=0;automatic_blocked=1;}publish_status();}
 poll_worker();menu_request();
}
static void disabled_step(struct ub_module *m){
 (void)m;if(!started){initialize(0);return;}
 if(module_enabled){module_enabled=0;automatic_pending=0;cleanup_pending=pending.active&&pending.operation!=CPU_RECONCILE;publish_status();
  if(publish_owner(0,0)||(!pending.active&&begin(CPU_RECONCILE,0,0,0,0))){effective=-1;automatic_blocked=1;}}
 poll_worker();
}
static int command(int argc,char **argv){if(argc>=2&&!strcmp(argv[1],"--cpu-worker"))return argc==3?worker(argv[2]):2;return -1;}
static const char *const choices[]={"off","on",0};
static const struct ub_option options[]={{"optimize",0,1,&optimize,choices},{0,0,0,0,0}};
static struct ub_part parts[]={{"control",0,0,0,0,0,step,{0},0,0}};
struct ub_module ub_module_cpu={ID,"CPU wait optimization (experimental)",0,options,start,parts,1,command,disabled_step};
