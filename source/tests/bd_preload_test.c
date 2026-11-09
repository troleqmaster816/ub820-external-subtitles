/* Exercise the real pre-main initializer with controlled memory I/O faults.
 * Actual ARM/glibc loader execution is covered separately by the loader fixture. */
#define _GNU_SOURCE
#define UB_BD_PRELOAD_TEST
#define UB_BD_DISC_ROOT "/tmp/ub820-bd-preload-test/disc"
#define UB_BD_EXE_HASH_182 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define open bd_test_open
#define pread bd_test_pread
#define pwrite bd_test_pwrite
#include "../modules/region/bd_preload.c"
#undef open
#undef pread
#undef pwrite
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
extern int open(const char *,int,...);
extern ssize_t pread(int,void *,size_t,off_t),pwrite(int,const void *,size_t,off_t);

static int failures,memfd=-1,writes,reads,fail_write,fail_rollback,interrupt_read,interrupt_write,partial_write,corrupt_readback,mount_opens;
static unsigned char memory[3][2];
#define CHECK(c) do{if(!(c)){printf("FAIL bd_preload_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
static int site(off_t at,size_t *offset){
 static const unsigned long addresses[]={UB_BD_SOURCE0,UB_BD_SOURCE1,UB_BD_SOURCE2};
 for(int i=0;i<3;i++)if(at>=(off_t)addresses[i]&&at<(off_t)addresses[i]+2){*offset=(size_t)(at-(off_t)addresses[i]);return i;}
 return -1;
}
int bd_test_open(const char *p,int flags,...){
 mode_t mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
 if(!strcmp(p,"/proc/self/mem")){memfd=open("/dev/null",O_RDWR);return memfd;}
 if(!strcmp(p,"/proc/self/exe"))p="/tmp/ub820-bd-preload-test/identity";
 if(!strcmp(p,"/proc/mounts"))mount_opens++;
 return open(p,flags,mode);
}
ssize_t bd_test_pread(int fd,void *b,size_t n,off_t at){
 size_t off;int i;
 if(fd!=memfd)return pread(fd,b,n,at);
 if(interrupt_read){interrupt_read=0;errno=EINTR;return -1;}
 reads++;i=site(at,&off);if(i<0||n>2-off){errno=EIO;return -1;}
 memcpy(b,memory[i]+off,n);
 if(corrupt_readback&&writes){((unsigned char *)b)[0]^=1;corrupt_readback=0;}
 return (ssize_t)n;
}
ssize_t bd_test_pwrite(int fd,const void *b,size_t n,off_t at){
 size_t off;int i;
 if(fd!=memfd)return pwrite(fd,b,n,at);
 if(interrupt_write){interrupt_write=0;errno=EINTR;return -1;}
 writes++;if(writes==fail_write||(fail_rollback&&writes>fail_write)){errno=EIO;return -1;}
 i=site(at,&off);if(i<0||n>2-off){errno=EIO;return -1;}
 if(partial_write&&writes==1)n=1;
 memcpy(memory[i]+off,b,n);return (ssize_t)n;
}
static void reset(void){
 static const unsigned char stock[3][2]={{1,0x23},{2,0x23},{4,0x23}};
 memcpy(memory,stock,sizeof memory);writes=reads=fail_write=fail_rollback=interrupt_read=interrupt_write=partial_write=corrupt_readback=mount_opens=0;
}
static int stock(void){return memory[0][0]==1&&memory[1][0]==2&&memory[2][0]==4;}
static void put(const char *p,const void *b,size_t n){int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);if(fd>=0){CHECK(write(fd,b,n)==(ssize_t)n);close(fd);}}
int main(void){
 char policy[16];unsigned char movie[78]={0};pid_t pid;int status;
 mkdir("/tmp/ub820-bd-preload-test",0700);mkdir(UB_BD_DISC_ROOT,0700);mkdir(UB_BD_DISC_ROOT "/BDMV",0700);
 put("/tmp/ub820-bd-preload-test/identity","",0);
 for(int value=1;value<=7;value++){if(value!=1&&value!=2&&value!=4&&value!=7)continue;
  reset();CHECK(!preload_patch(value,0));CHECK(memory[0][0]==value&&memory[1][0]==value&&memory[2][0]==value);
 }
 for(int i=0;i<3;i++){reset();memory[i][1]=0;CHECK(preload_patch(7,0)==-1&&writes==0);}
 reset();fail_write=2;CHECK(preload_patch(7,0)==-1&&stock());
 reset();partial_write=1;fail_write=2;CHECK(preload_patch(7,0)==-1&&stock());
 reset();corrupt_readback=1;CHECK(preload_patch(7,0)==-1&&stock());
 reset();fail_write=2;fail_rollback=1;CHECK(preload_patch(7,0)==-2);
 reset();interrupt_read=interrupt_write=1;CHECK(!preload_patch(7,0)&&memory[0][0]==7&&memory[2][0]==7);
 reset();CHECK(!preload_patch(-1,5)&&memory[0][0]==1&&memory[1][0]==1&&memory[2][0]==4);
 /* The constructor restores preexisting user preloads and removes private
  * variables before native code may launch further children. */
 reset();setenv("UB820_BD_POLICY","v1:7",1);setenv("UB820_BD_PRELOAD_OLD","user-first.so user-second.so",1);setenv("LD_PRELOAD","ours.so",1);
 ub_bd_preload_init();CHECK(memory[0][0]==7&&memory[1][0]==7&&memory[2][0]==7);
 CHECK(!strcmp(getenv("LD_PRELOAD"),"user-first.so user-second.so"));CHECK(!getenv("UB820_BD_POLICY")&&!getenv("UB820_BD_PRELOAD_OLD"));
 reset();setenv("UB820_BD_POLICY","v1:1",1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();CHECK(!getenv("LD_PRELOAD")&&memory[1][0]==1);
 /* Wrong identity or policy never writes executable memory. */
 put("/tmp/ub820-bd-preload-test/identity","changed",7);
 reset();setenv("UB820_BD_POLICY","v1:7",1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();CHECK(stock()&&!writes);
 put("/tmp/ub820-bd-preload-test/identity","",0);
 reset();setenv("UB820_BD_POLICY","v1:garbage",1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();CHECK(stock()&&!writes);
 /* Auto uses files from this execution, then keeps stock after media changes.
  * MOVE r1, PSR20; AND r1, 2. */
 put(UB_BD_DISC_ROOT "/BDMV/index.bdmv","INDX0200xxxx",12);
 memcpy(movie,"MOBJ0200",8);movie[49]=1;movie[53]=2;
 {const unsigned char commands[24]={0x50,0,0,1,0,0,0,1,0x80,0,0,0x14,0x50,0x40,0,9,0,0,0,1,0,0,0,2};memcpy(movie+54,commands,sizeof commands);}
 put(UB_BD_DISC_ROOT "/BDMV/MovieObject.bdmv",movie,sizeof movie);CHECK(preload_auto_mask()==2);
 reset();setenv("UB820_BD_POLICY","v1:auto",1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();CHECK(memory[0][0]==2&&memory[1][0]==2&&memory[2][0]==2);
 movie[49]=2;put(UB_BD_DISC_ROOT "/BDMV/MovieObject.bdmv",movie,sizeof movie);mount_opens=0;CHECK(!preload_auto_mask()&&!mount_opens); /* primary disc, truncated list: never borrow another mount */
 unlink(UB_BD_DISC_ROOT "/BDMV/MovieObject.bdmv");
 reset();setenv("UB820_BD_POLICY","v1:auto",1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();CHECK(stock()&&!writes);
 /* Unrecoverable partial mutation prevents reaching the native entry point. */
 pid=fork();CHECK(pid>=0);if(!pid){reset();fail_write=2;fail_rollback=1;snprintf(policy,sizeof policy,"v1:7");setenv("UB820_BD_POLICY",policy,1);setenv("UB820_BD_PRELOAD_OLD","",1);ub_bd_preload_init();_exit(99);}
 CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==125);
 printf("bd_preload_test %s\n",failures?"FAILED":"passed");return failures?1:0;
}
