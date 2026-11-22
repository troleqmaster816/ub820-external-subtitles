/* ufs: copy one file between the USB stick and /tmp through the player's own
 * file system library.
 *
 *   ufs read  <stick path> <tmp path>     e.g. /mnt/usb/sub/modules.cfg
 *   ufs load  <stick path> <tmp path>     a file written by "ufs write"
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
 *   int UFS_OpenDir(const char *path,uint32_t *handle), UFS_CloseDir(handle)
 *   int UFS_ReadDir(uint32_t handle,void *record,uint32_t *count): count 0 at
 *     the end; type at +10 (4 directory, 8 file), name at +11 (as in
 *     external_subtitles/backend.c)
 *   int UFS_Rename(const char *from,const char *to), UFS_Unlink(const char *path)
 * libufs.so of 1.85 has the same code as 1.82 (every section but the prelink
 * data is byte-identical), so the same calls hold there.
 *
 * Writing never truncates the file on the stick, and never relies on the file
 * layer replacing an existing file: DFAT's replacing rename parks the old file
 * under a name of its own choosing (<stem>.tp0, or .tp1, .tp2 after
 * collisions; found by emulation), which a reader cannot tie to one target.
 * The tool takes its own steps with names it controls:
 *   1. the data goes to <to>.new, which is written and closed;
 *   2. an existing <to> is renamed to <to>.old (a stale <to>.old is deleted
 *      first, only while <to> itself opens, so the last good copy is never
 *      the one removed);
 *   3. <to>.new is renamed to <to>, which no longer exists;
 *   4. <to>.old is deleted.
 * "load" is the read for a file this tool writes. It reads every state an
 * interruption can leave, once the directory confirms whether <to> exists:
 *   <to> (+ any leftover)        <to>, complete: it only ever comes from a
 *                                rename of a closed file;
 *   <to>.old, no <to>            <to>.old, the previous complete file;
 *   <to>.new alone, or nothing   absent: no complete file was ever there.
 * "read" is for files the helper only reads (modules.cfg, SSH files): no copy
 * is ever taken in place of the file itself.
 * An open failure does not show that a file is absent (the file layer maps
 * several statuses to one error); only a complete listing of its directory
 * without the name, its .old, a DFAT <stem>.tpN, or an 8.3 alias that may
 * stand for any of them does. Anything that may be a parked copy makes the
 * result "cannot open" (4), never "absent".
 *
 * Exit codes: 0 done, 2 usage, 3 UFS_Init, 4 source open, 5 destination open,
 * 6 read, 7 write, 8 close/rename, 9 file larger than the limit, 12 the source
 * is not on the stick (read and load only).
 */
typedef unsigned int u32;
typedef unsigned long long u64;

extern int UFS_Init(void);
extern int UFS_Open(const char *path,u32 flags,u32 mode,u32 *handle);
extern int UFS_Read(u32 handle,void *buf,u64 size,u64 *done);
extern int UFS_Write(u32 handle,const void *buf,u64 size,u64 *done);
extern int UFS_Close(u32 handle);
extern int UFS_OpenDir(const char *path,u32 *handle);
extern int UFS_ReadDir(u32 handle,void *record,u32 *count);
extern int UFS_CloseDir(u32 handle);
extern int UFS_Rename(const char *from,const char *to);
extern int UFS_Unlink(const char *path);
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
  unsigned v=code<0?0u-(unsigned)code:(unsigned)code;char digits[12];int d=0;
  line[n++]=' ';line[n++]='(';if(code<0)line[n++]='-';
  do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);
  while(d)line[n++]=digits[--d];
  line[n++]=')';
 }
 line[n++]='\n';write(2,line,n);
}

/* out = path + suffix; 0, or -1 if it does not fit. */
static int suffixed(char *out,unsigned size,const char *path,const char *suffix){
 unsigned n=length(path),k=length(suffix);
 if(n+k>=size)return -1;
 for(unsigned i=0;i<n;i++)out[i]=path[i];
 for(unsigned i=0;i<=k;i++)out[n+i]=suffix[i];
 return 0;
}
static char upper(char c){return c>='a'&&c<='z'?(char)(c-32):c;}
static int same_name(const char *a,const char *b){while(*a&&upper(*a)==upper(*b)){a++;b++;}return !*a&&!*b;}
/* The extension (after the last dot), up to 3 characters as in an 8.3 name. */
static const char *extension(const char *s){const char *dot=0;for(;*s;s++)if(*s=='.')dot=s+1;return dot?dot:s;}
static int same_ext(const char *a,const char *b){for(int i=0;i<3;i++){if(upper(a[i])!=upper(b[i]))return 0;if(!a[i])return 1;}return 1;}
static int is_tp(const char *e){return upper(e[0])=='T'&&upper(e[1])=='P'&&e[2]>='0'&&e[2]<='9'&&!e[3];}
/* FAT can use the first eight stem characters without a numeric tail.
 * All helper filenames are ASCII. A dot inside the stem is omitted. */
static int short_stem(const char *entry,const char *name,const char *end){
 unsigned n=0;
 while(name<end&&n<8){
  char c=*name++;
  if(c=='.'||c==' ')continue;
  if(upper(entry[n])!=upper(c))return 0;
  n++;
 }
 return n&&entry[n]=='.';
}
static int hex_digit(char c){c=upper(c);return (c>='0'&&c<='9')||(c>='A'&&c<='F');}
/* A FAT numeric-tail alias of the stem [name,end): at most 8 characters
 * before the dot, <prefix>~<1-6 digits, no leading 0>. The prefix is a prefix
 * of the stem with dots and spaces dropped, or Windows' hashed form: 1-2 of
 * its characters and up to 4 hex digits. Any other '~' belongs to a real
 * long name (UFS_ReadDir lists long names) and never stands for ours. */
static int tail_alias(const char *entry,const char *name,const char *end){
 char stem[16];unsigned n=0,t,k,j,p;
 for(const char *c=name;c<end&&n<sizeof stem;c++)if(*c!='.'&&*c!=' ')stem[n++]=upper(*c);
 for(t=0;entry[t]&&entry[t]!='.'&&entry[t]!='~';t++){}
 if(!t||entry[t]!='~'||entry[t+1]<'1'||entry[t+1]>'9')return 0;
 for(k=t+1;entry[k]>='0'&&entry[k]<='9';k++){}
 if((entry[k]&&entry[k]!='.')||k>8)return 0;
 for(j=0;j<t&&j<n&&upper(entry[j])==stem[j];j++){}
 if(j==t)return 1;
 if(!j||t>6)return 0;
 for(p=j<2?j:2;p<t&&hex_digit(entry[p]);p++){}
 return p==t&&t-(j<2?j:2)<=4;
}
/* entry may be the exact name or an 8.3 alias of a current or parked file. */
enum file_names { TARGET=1, PREVIOUS=2, PARKED=4 };
static int matching_names(const char *entry,const char *name){
 const char *e,*dot=0;
 if(same_name(entry,name))return TARGET;
 {const char *a=entry,*b=name;while(*b&&upper(*a)==upper(*b)){a++;b++;}if(!*b&&same_name(a,".old"))return PREVIOUS;}
 for(e=name;*e;e++)if(*e=='.')dot=e;
 if(dot){
  unsigned k=(unsigned)(dot-name),j;
  for(j=0;j<k&&entry[j]&&upper(entry[j])==upper(name[j]);j++){}
  if(j==k&&entry[k]=='.'&&is_tp(entry+k+1))return PARKED;
 }
 if(upper(entry[0])!=upper(name[0]))return 0;
 e=extension(entry);
 const char *stem_end=dot?dot:name+length(name);
 int base=tail_alias(entry,name,stem_end)||short_stem(entry,name,stem_end);
 int backup=tail_alias(entry,name,name+length(name))||short_stem(entry,name,name+length(name));
 return (base&&same_ext(e,extension(name))?TARGET:0)|(backup&&same_ext(e,"OLD")?PREVIOUS:0)|(base&&is_tp(e)?PARKED:0);
}
/* Possible names in a complete listing, or -1 for an incomplete observation. */
static int listed(const char *path){
 static unsigned char record[1024];char dir[300];unsigned n=length(path),i,k;u32 h,count;int rc=0,found=0;
 for(i=n;i&&path[i-1]!='/';i--){}
 if(!i||i>=sizeof dir||!path[i])return -1;
 k=i>1?i-1:1;for(unsigned j=0;j<k;j++)dir[j]=path[j];dir[k]=0;
 if(UFS_OpenDir(dir,&h))return -1;
 for(n=0;n<8192;n++){
  for(unsigned j=0;j<sizeof record;j++)record[j]=0;
  count=0;
  if((rc=UFS_ReadDir(h,record,&count))||!count)break;
  record[sizeof record-1]=0;
  found|=matching_names((const char *)record+11,path+i);
 }
 if(UFS_CloseDir(h)||rc||n==8192)return -1;
 return found;
}

static int read_stick(const char *from,const char *to,int saved){
 static char buf[CHUNK];char temp[300],old[300];u32 h;u64 got;unsigned total=0;int rc,fd;
 if(suffixed(temp,sizeof temp,to,".ufs")||suffixed(old,sizeof old,from,".old"))return 2;
 if((rc=UFS_Open(from,O_RDONLY,0,&h))){
  int names=listed(from);
  if(!names){say("not on stick: ",from,0);return 12;}
  if(!saved||names!=PREVIOUS||UFS_Open(old,O_RDONLY,0,&h)){say("cannot open on stick ",from,rc);return 4;}
  say("replacement was interrupted; reading the previous copy ",old,0);
 }
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);
 if(fd<0){UFS_Close(h);say("cannot create ",temp,0);return 5;}
 for(;;){
  got=0;
  if((rc=UFS_Read(h,buf,CHUNK,&got))){close(fd);unlink(temp);UFS_Close(h);say("read failed on ",from,rc);return 6;}
  if(!got)break;
  if(got>CHUNK||(total+=(unsigned)got)>LIMIT){close(fd);unlink(temp);UFS_Close(h);say("file too large: ",from,0);return 9;}
  if(write(fd,buf,(unsigned)got)!=(int)got){close(fd);unlink(temp);UFS_Close(h);say("cannot write ",temp,0);return 7;}
 }
 if((rc=UFS_Close(h))){close(fd);unlink(temp);say("close failed on ",from,rc);return 8;}
 if(close(fd)||rename(temp,to)){unlink(temp);say("cannot finish ",to,0);return 8;}
 return 0;
}

static int write_stick(const char *from,const char *to){
 static char buf[CHUNK];char temp[300],old[300];u32 h;u64 done;unsigned total=0;int rc,fd,n;
 if(suffixed(temp,sizeof temp,to,".new")||suffixed(old,sizeof old,to,".old"))return 2;
 fd=open(from,O_RDONLY);if(fd<0){say("cannot open ",from,0);return 4;}
 if((rc=UFS_Open(temp,O_WRONLY|O_CREAT|O_TRUNC,0644,&h))){close(fd);say("cannot create on stick ",temp,rc);return 5;}
 while((n=read(fd,buf,CHUNK))>0){
  if((total+=(unsigned)n)>LIMIT){close(fd);UFS_Close(h);UFS_Unlink(temp);say("file too large: ",from,0);return 9;}
  done=0;
  if((rc=UFS_Write(h,buf,(u64)n,&done))||done!=(u64)n){close(fd);UFS_Close(h);UFS_Unlink(temp);say("write failed on ",temp,rc);return 7;}
 }
 close(fd);
 if(n<0){UFS_Close(h);UFS_Unlink(temp);say("cannot read ",from,0);return 6;}
 if((rc=UFS_Close(h))){UFS_Unlink(temp);say("close failed on ",temp,rc);return 8;}
 int moved=0;
 if(!UFS_Open(to,O_RDONLY,0,&h)){
  if((rc=UFS_Close(h))){say("close failed on ",to,rc);return 8;}
  /* A stale backup is expendable only with a readable current file.
   * Unlink also fails for an absent name, so consult the directory then. */
  if((rc=UFS_Unlink(old))){
   int names=listed(to);
   if(names<0||(names&PREVIOUS)){say("cannot clear previous copy ",old,rc);return 8;}
  }
  if((rc=UFS_Rename(to,old))){say("cannot move aside ",to,rc);return 8;}
  moved=1;
 }else{
  int names=listed(to);
  if(names!=0&&names!=PREVIOUS){say("cannot establish missing destination ",to,0);return 8;}
 }
 if((rc=UFS_Rename(temp,to))){
  /* A failure with an uncertain outcome must leave the backup in place. */
  if(moved&&listed(to)==PREVIOUS)UFS_Rename(old,to);
  say("cannot replace ",to,rc);return 8;
 }
 UFS_Unlink(old); /* the previous copy an interrupted write left, now replaced */
 return 0;
}

static int same(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
int ufs_main(int argc,char **argv){
 int rc;
 if(argc!=4||(!same(argv[1],"read")&&!same(argv[1],"load")&&!same(argv[1],"write"))){say("usage: ufs read|load|write <from> <to>",0,0);return 2;}
 if((rc=UFS_Init())){say("UFS_Init failed",0,rc);return 3;}
 if(same(argv[1],"write"))return write_stick(argv[2],argv[3]);
 return read_stick(argv[2],argv[3],same(argv[1],"load"));
}

/* Entry without libc start files: the dynamic linker has already run the
 * libraries' initialisers; argc/argv are on the initial stack. */
#ifndef UB_HOST_TEST
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
#endif
