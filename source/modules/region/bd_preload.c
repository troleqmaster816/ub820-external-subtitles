/* Normal ELF-loader constructor, not a ptrace payload. The verified launcher
 * loads this only for future apl_bdv starts. The native executable has no
 * preinit array; its region initialization is reached after loader constructors.
 * Thus all three source instructions are committed before the first native
 * region computation, without racing process detection or rewriting registers.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "bd_policy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bd_sha256.h"

#ifndef UB_BD_EXE_HASH_182
#define UB_BD_EXE_HASH_182 "0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d"
#endif
#ifndef UB_BD_EXE_HASH_185
#define UB_BD_EXE_HASH_185 "07f150cf640cf63a91a25994197ae26fb91f755e38b8a8313ee9c00a9280899f"
#endif
#ifndef UB_BD_SOURCE0
#define UB_BD_SOURCE0 0x4a738u
#define UB_BD_SOURCE1 0x4a7bcu
#define UB_BD_SOURCE2 0x4a7c2u
#endif
#ifndef UB_BD_DISC_ROOT
#define UB_BD_DISC_ROOT "/mnt/odd"
#endif
#define BD_FILE_LIMIT (1024*1024)

static void preload_log(const char *s){
 char line[256];int n=snprintf(line,sizeof line,"region: BD pre-main PID%d: %s\n",(int)getpid(),s);
 int fd;if(n<=0||n>=(int)sizeof line)return;
 fd=open("/tmp/ub820_submenu.log",O_WRONLY|O_APPEND|O_CLOEXEC);
 if(fd>=0){ssize_t r;do{r=write(fd,line,(size_t)n);}while(r<0&&errno==EINTR);close(fd);}
}
static int preload_identity(void){
 struct hash h;unsigned char buf[8192];char digest[65];ssize_t n;int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
 if(fd<0)return -1;
 hash_init(&h);
 for(;;){n=read(fd,buf,sizeof buf);if(n<0&&errno==EINTR)continue;if(n<=0)break;hash_feed(&h,buf,(size_t)n);}
 close(fd);if(n<0)return -1;hash_end(&h,digest);
 return strcmp(digest,UB_BD_EXE_HASH_182)&&strcmp(digest,UB_BD_EXE_HASH_185)?-1:0;
}
static unsigned char *preload_read(const char *path,size_t *size){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);off_t length;unsigned char *b;size_t used=0;ssize_t n;
 if(fd<0)return 0;
 length=lseek(fd,0,SEEK_END);
 if(length<=0||length>BD_FILE_LIMIT||lseek(fd,0,SEEK_SET)<0){close(fd);return 0;}
 b=malloc((size_t)length);if(!b){close(fd);return 0;}
 while(used<(size_t)length){n=read(fd,b+used,(size_t)length-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 close(fd);if(used!=(size_t)length){free(b);return 0;}*size=used;return b;
}
static unsigned preload_movie_mask(const char *root){
 char path[512];unsigned char *b;size_t size,at;unsigned objects,o,mask=0;int n;
 n=snprintf(path,sizeof path,"%s/BDMV/MovieObject.bdmv",root);if(n<0||n>=(int)sizeof path)return 0;
 b=preload_read(path,&size);if(!b)return 0;
 if(size<50||memcmp(b,"MOBJ",4)){free(b);return 0;}
 objects=ub_bd_be16(b+48);at=50;
 for(o=0;o<objects;o++){
  if(at+4>size){mask=0;break;}
  unsigned cmds=ub_bd_be16(b+at+2);at+=4;
  if(size-at<12*(size_t)cmds){mask=0;break;}
  mask|=ub_bd_hdmv_mask(b+at,cmds);at+=12*(size_t)cmds;
 }
 free(b);return mask;
}
/* Analyse the current mount in this very execution, never a cached decision
 * from an earlier disc. Missing files keep stock; auto remains experimental. */
static int preload_is_bd(const char *root){
 char path[512];unsigned char b[12];size_t used=0;ssize_t got;int n,fd;
 n=snprintf(path,sizeof path,"%s/BDMV/index.bdmv",root);if(n<0||n>=(int)sizeof path)return 0;
 fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
 if(fd<0)return errno==ENOENT||errno==ENOTDIR?0:-1;
 while(used<sizeof b){got=read(fd,b+used,sizeof b-used);if(got<0&&errno==EINTR)continue;if(got<=0)break;used+=(size_t)got;}
 close(fd);return used==sizeof b&&!memcmp(b,"INDX",4)?1:-1;
}
static unsigned preload_auto_mask(void){
 char *line,*save;int fd,state;char buf[16384];size_t used=0;ssize_t n;
 state=preload_is_bd(UB_BD_DISC_ROOT);if(state)return state>0?preload_movie_mask(UB_BD_DISC_ROOT):0;
 fd=open("/proc/mounts",O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
 while(used<sizeof buf-1){n=read(fd,buf+used,sizeof buf-1-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=(size_t)n;}
 close(fd);if(used==sizeof buf-1||n<0)return 0;buf[used]=0;
 for(line=strtok_r(buf,"\n",&save);line;line=strtok_r(0,"\n",&save)){
  char dev[200],root[200],type[40];
  if(sscanf(line,"%199s %199s %39s",dev,root,type)!=3||(strcmp(type,"udf")&&strcmp(type,"iso9660"))||!strcmp(root,UB_BD_DISC_ROOT))continue;
  state=preload_is_bd(root);if(state)return state>0?preload_movie_mask(root):0;
 }
 return 0;
}
static int preload_mem_read(int fd,unsigned long address,void *buffer,size_t size){
 size_t done=0;ssize_t n;
 while(done<size){n=pread(fd,(char *)buffer+done,size-done,(off_t)(address+done));if(n<0&&errno==EINTR)continue;if(n<=0)return -1;done+=(size_t)n;}return 0;
}
static int preload_mem_write(int fd,unsigned long address,const void *buffer,size_t size){
 size_t done=0;ssize_t n;
 while(done<size){n=pwrite(fd,(const char *)buffer+done,size-done,(off_t)(address+done));if(n<0&&errno==EINTR)continue;if(n<=0)return -1;done+=(size_t)n;}return 0;
}
/* Returns -2 only when the original instructions could not be recovered.
 * The constructor then exits; native code is never entered half-patched. */
static int preload_patch(int policy,unsigned mask){
 static const unsigned long address[3]={UB_BD_SOURCE0,UB_BD_SOURCE1,UB_BD_SOURCE2};
 static const unsigned char stock[3]={1,2,4};
 unsigned char before[3][2],want[3][2],check[2];unsigned i,written=0;int fd,rc=-1;
 fd=open("/proc/self/mem",O_RDWR|O_CLOEXEC);if(fd<0)return -1;
 for(i=0;i<3;i++){
  want[i][0]=(unsigned char)ub_bd_region_for(policy==-1?'M':'F',policy==-1?mask:(unsigned)policy,stock[i]);want[i][1]=0x23;
  if(preload_mem_read(fd,address[i],before[i],2)||before[i][1]!=0x23||before[i][0]!=stock[i])goto done;
 }
 for(i=0;i<3;i++){
  if(!memcmp(before[i],want[i],2))continue;
  written=i+1;
  /* The vendor proc/mem VM_EXEC path synchronizes the instruction cache. */
  if(preload_mem_write(fd,address[i],want[i],2)||preload_mem_read(fd,address[i],check,2)||memcmp(check,want[i],2))goto rollback;
 }
 rc=0;goto done;
 rollback:
 for(i=written;i>0;i--)if(preload_mem_write(fd,address[i-1],before[i-1],2)||preload_mem_read(fd,address[i-1],check,2)||memcmp(check,before[i-1],2)){rc=-2;goto done;}
 done:close(fd);return rc;
}
static int preload_policy(const char *s){
 if(!s)return 0;
 if(!strcmp(s,"v1:auto"))return -1;
 if(!strcmp(s,"v1:1"))return 1;
 if(!strcmp(s,"v1:2"))return 2;
 if(!strcmp(s,"v1:4"))return 4;
 if(!strcmp(s,"v1:7"))return 7;
 return 0;
}
#ifndef UB_BD_PRELOAD_TEST
__attribute__((constructor))
#endif
static void ub_bd_preload_init(void){
 int policy=preload_policy(getenv("UB820_BD_POLICY")),rc;unsigned mask=0;const char *old=getenv("UB820_BD_PRELOAD_OLD");
 if(!old)return; /* Not launched by our verified media-manager hook. */
 /* Only this child's environment changes. Never propagate our initializer
  * into tools or further executables started by the player. */
 if(*old){if(setenv("LD_PRELOAD",old,1)){preload_log("cannot restore inherited preload environment; refusing native main");_exit(125);}}
 else unsetenv("LD_PRELOAD");
 unsetenv("UB820_BD_POLICY");unsetenv("UB820_BD_PRELOAD_OLD");
 if(!policy)return;
 if(preload_identity()){preload_log("unrecognized executable; source unchanged");return;}
 if(policy==-1){mask=preload_auto_mask();if(!mask){preload_log("auto found no current HDMV mask; stock source retained");return;}}
 rc=preload_patch(policy,mask);
 if(rc==-2){preload_log("rollback could not be verified; refusing to enter native main");_exit(125);}
 if(rc){preload_log("source preflight/write failed; stock instructions retained");return;}
 preload_log(policy==-1?"current-disc HDMV policy installed before native main":"selected region installed before native main");
}
