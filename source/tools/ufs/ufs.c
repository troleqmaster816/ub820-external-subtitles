/* ufs: copy one file between the USB stick and /tmp through the player's own
 * file system library.
 *
 *   ufs read  <stick path> <tmp path>     e.g. /mnt/usb/sub/modules.cfg
 *   ufs write <tmp path> <stick path>
 *
 * The player never mounts the stick in Linux; every program reaches it through
 * libufs (a user-space FAT/UDF layer over /dev/dtvrec/rtsc/um*). This tool uses
 * it the way apl_meda_bdplay reads sub.srt (UFS_Init, UFS_Open, UFS_Read,
 * UFS_Close) and apl_medm writes files (UFS_Write).
 *
 * It is linked against the stock 1.82 libc 2.20 and libufs.so, with its own
 * _start and no libc headers, so no newer symbol version or fortify call is
 * referenced. Signatures from the 1.82 analysis:
 *   int UFS_Open(const char *path,uint32_t flags,uint32_t mode,uint32_t *handle)
 *   int UFS_Read/UFS_Write(uint32_t handle,void *buf,uint64_t size,uint64_t *done)
 *   int UFS_Close(uint32_t handle)
 * Exit codes: 0 done, 2 usage, 3 UFS_Init, 4 source open, 5 destination open,
 * 6 read, 7 write, 8 close/rename, 9 file larger than the limit.
 */
typedef unsigned int u32;
typedef unsigned long long u64;

extern int UFS_Init(void);
extern int UFS_Open(const char *path,u32 flags,u32 mode,u32 *handle);
extern int UFS_Read(u32 handle,void *buf,u64 size,u64 *done);
extern int UFS_Write(u32 handle,const void *buf,u64 size,u64 *done);
extern int UFS_Close(u32 handle);
extern int open(const char *path,int flags,...);
extern int read(int fd,void *buf,unsigned n);
extern int write(int fd,const void *buf,unsigned n);
extern int close(int fd);
extern int rename(const char *from,const char *to);
extern int unlink(const char *path);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define CHUNK 4096u
#define LIMIT (1024u*1024u)

static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void say(const char *a,const char *b,int code){
 char line[400];unsigned n=0,i;const char *parts[3]={"ufs tool: ",a,b};
 for(i=0;i<3;i++)for(const char *p=parts[i];p&&*p&&n<sizeof line-20;p++)line[n++]=*p;
 if(code){
  unsigned v=(unsigned)(code<0?-code:code);char digits[12];int d=0;
  line[n++]=' ';line[n++]='(';if(code<0)line[n++]='-';
  do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);
  while(d)line[n++]=digits[--d];
  line[n++]=')';
 }
 line[n++]='\n';write(2,line,n);
}

static int read_stick(const char *from,const char *to){
 static char buf[CHUNK];char temp[300];u32 h;u64 got;unsigned total=0,n;int rc,fd;
 n=length(to);if(n+5>=sizeof temp)return 2;
 for(unsigned i=0;i<n;i++)temp[i]=to[i];
 temp[n]='.';temp[n+1]='u';temp[n+2]='f';temp[n+3]='s';temp[n+4]=0;
 if((rc=UFS_Open(from,O_RDONLY,0,&h))){say("cannot open on stick ",from,rc);return 4;}
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);
 if(fd<0){UFS_Close(h);say("cannot create ",temp,0);return 5;}
 for(;;){
  got=0;
  if((rc=UFS_Read(h,buf,CHUNK,&got))){close(fd);unlink(temp);UFS_Close(h);say("read failed on ",from,rc);return 6;}
  if(!got)break;
  if(got>CHUNK||(total+=(unsigned)got)>LIMIT){close(fd);unlink(temp);UFS_Close(h);say("file too large: ",from,0);return 9;}
  if(write(fd,buf,(unsigned)got)!=(int)got){close(fd);unlink(temp);UFS_Close(h);say("cannot write ",temp,0);return 7;}
 }
 UFS_Close(h);
 if(close(fd)||rename(temp,to)){unlink(temp);say("cannot finish ",to,0);return 8;}
 return 0;
}

static int write_stick(const char *from,const char *to){
 static char buf[CHUNK];u32 h;u64 done;unsigned total=0;int rc,fd,n;
 fd=open(from,O_RDONLY);if(fd<0){say("cannot open ",from,0);return 4;}
 if((rc=UFS_Open(to,O_WRONLY|O_CREAT|O_TRUNC,0644,&h))){close(fd);say("cannot create on stick ",to,rc);return 5;}
 while((n=read(fd,buf,CHUNK))>0){
  if((total+=(unsigned)n)>LIMIT){rc=0;close(fd);UFS_Close(h);say("file too large: ",from,0);return 9;}
  done=0;
  if((rc=UFS_Write(h,buf,(u64)n,&done))||done!=(u64)n){close(fd);UFS_Close(h);say("write failed on ",to,rc);return 7;}
 }
 close(fd);
 if(n<0){UFS_Close(h);say("cannot read ",from,0);return 6;}
 if((rc=UFS_Close(h))){say("close failed on ",to,rc);return 8;}
 return 0;
}

static int same(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
int ufs_main(int argc,char **argv){
 int rc;
 if(argc!=4||(!same(argv[1],"read")&&!same(argv[1],"write"))){say("usage: ufs read|write <from> <to>",0,0);return 2;}
 if((rc=UFS_Init())){say("UFS_Init failed",0,rc);return 3;}
 return same(argv[1],"read")?read_stick(argv[2],argv[3]):write_stick(argv[2],argv[3]);
}

/* Entry without libc start files: the dynamic linker has already run the
 * libraries' initialisers; argc/argv are on the initial stack. */
__asm__(
 ".text\n.arm\n.align 2\n.global _start\n.type _start,%function\n"
 "_start:\n"
 " mov fp,#0\n"
 " mov lr,#0\n"
 " ldr r0,[sp]\n"
 " add r1,sp,#4\n"
 " bic sp,sp,#7\n"
 " blx ufs_main\n"
 " mov r7,#248\n"   /* exit_group(r0) */
 " svc #0\n"
);
