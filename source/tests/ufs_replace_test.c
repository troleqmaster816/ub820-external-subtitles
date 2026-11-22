/* Host test of the UFS tool's write, read and load on a directory standing in
 * for the stick. The file layer here behaves like the player's DFAT rename
 * (found by emulation of the stock code): an existing target is moved to
 * <stem>.tp0 (its extension replaced; .tp1, .tp2 when taken), the new file
 * moved in, the parked copy deleted; a failed second move is undone. Checked:
 * the tool never relies on that replacing rename, a failed or oversized write
 * never touches the last good copy, "load" reads every state an interrupted
 * write leaves correctly, "read" never takes a copy in place of the file, and
 * only a complete directory listing without the name or any copy of it gives
 * "absent" (12), never an open error (4). */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int ufs_main(int,char **);
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL ufs_replace_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)

static int fail_write,fail_second_move,fail_move_aside,no_replace,fail_opendir,fail_readdir_after=-1,replacing_renames;
static const char *fail_open; /* UFS_Open fails for this exact path */
static const char *fail_unlink;
static const char *extra_entry; /* listed although no such file exists */
static int exists(const char *p){struct stat st;return !stat(p,&st);}
int UFS_Init(void){return 0;}
int UFS_Open(const char *path,unsigned flags,unsigned mode,unsigned *h){
 int fd;
 if(fail_open&&!strcmp(path,fail_open))return -10106;
 fd=open(path,(int)flags,mode);if(fd<0)return -10106;
 *h=(unsigned)fd;return 0;
}
int UFS_Read(unsigned h,void *buf,unsigned long long n,unsigned long long *done){
 ssize_t r=read((int)h,buf,(size_t)n);if(r<0)return -1;*done=(unsigned long long)r;return 0;
}
int UFS_Write(unsigned h,const void *buf,unsigned long long n,unsigned long long *done){
 ssize_t r;if(fail_write)return -7;
 r=write((int)h,buf,(size_t)n);if(r<0)return -1;*done=(unsigned long long)r;return 0;
}
int UFS_Close(unsigned h){return close((int)h)?-1:0;}
int UFS_Unlink(const char *path){if(fail_unlink&&!strcmp(path,fail_unlink))return -1;return unlink(path)?-1:0;}
int UFS_Rename(const char *from,const char *to){
 char old[400];const char *dot=strrchr(to,'.');int n;
 int moving_new=fail_second_move&&strstr(from,".new")!=0; /* that move fails */
 if(!exists(from))return -1;
 if(fail_move_aside&&strstr(to,".old"))return -1;
 if(!exists(to))return moving_new||rename(from,to)?-1:0;
 if(no_replace)return -41990;
 replacing_renames++;
 for(n=0;n<10;n++){snprintf(old,sizeof old,"%.*s.tp%d",(int)(dot?dot-to:(long)strlen(to)),to,n);if(!exists(old))break;}
 if(rename(to,old))return -1;
 if(moving_new){if(rename(old,to)){}return -41217;}
 if(rename(from,to))return -1;
 unlink(old);return 0;
}
static DIR *listing;static int listed_count;
int UFS_OpenDir(const char *path,unsigned *h){if(fail_opendir||!(listing=opendir(path)))return -1;listed_count=0;*h=1;return 0;}
int UFS_ReadDir(unsigned h,void *record,unsigned *count){
 struct dirent *e;unsigned char *r=record;(void)h;
 if(fail_readdir_after>=0&&listed_count>=fail_readdir_after)return -5;
 if(extra_entry&&listed_count==0){r[10]=8;strcpy((char *)r+11,extra_entry);listed_count++;*count=1;return 0;}
 do e=readdir(listing);while(e&&e->d_name[0]=='.');
 if(!e){*count=0;return 0;}
 r[10]=e->d_type==DT_DIR?4:8;snprintf((char *)r+11,0x120-11,"%s",e->d_name);listed_count++;*count=1;return 0;
}
int UFS_CloseDir(unsigned h){(void)h;closedir(listing);listing=0;return 0;}

static char dir[64],target[128],src[128],out[128],tp0[160],older[160],newer[160],other[160];
static void put(const char *path,const char *text){FILE *f=fopen(path,"w");CHECK(f!=0);if(f){fputs(text,f);fclose(f);}}
static int is(const char *path,const char *text){char buf[64]={0};FILE *f=fopen(path,"r");size_t n=f?fread(buf,1,sizeof buf-1,f):0;if(f)fclose(f);return f&&n==strlen(text)&&!memcmp(buf,text,n);}
static int tool(const char *op,const char *a,const char *b){char *argv[]={"ufs",(char *)op,(char *)a,(char *)b,0};return ufs_main(4,argv);}
static int no_leftovers(void){return !exists(newer)&&!exists(older)&&!exists(tp0);}

int main(void){
 snprintf(dir,sizeof dir,"/tmp/ub820-ufs-replace-XXXXXX");if(!mkdtemp(dir))return 1;
 snprintf(target,sizeof target,"%s/ub820_menu.cfg",dir);snprintf(src,sizeof src,"%s/ram.cfg",dir);snprintf(out,sizeof out,"%s/out.cfg",dir);
 snprintf(tp0,sizeof tp0,"%s/ub820_menu.tp0",dir);snprintf(older,sizeof older,"%s.old",target);snprintf(newer,sizeof newer,"%s.new",target);
 snprintf(other,sizeof other,"%s/modules.cfg",dir);
 /* A write error leaves the previous file whole (the old tool emptied it). */
 put(target,"previous");put(src,"new");fail_write=1;
 CHECK(tool("write",src,target)==7);CHECK(is(target,"previous"));CHECK(!exists(newer));fail_write=0;
 /* Over the 1 MB limit: refused, previous file whole. */
 {FILE *f=fopen(src,"w");if(f){for(int i=0;i<1024*1024+1;i++)fputc('x',f);fclose(f);}}
 CHECK(tool("write",src,target)==9);CHECK(is(target,"previous"));CHECK(!exists(newer));
 /* The final move fails: the previous file is moved back. */
 put(src,"new");fail_second_move=1;
 CHECK(tool("write",src,target)==8);CHECK(is(target,"previous"));CHECK(!exists(older));fail_second_move=0;
 /* Normal replacement, by the tool's own steps: the file layer never parks
  * a copy under a name of its own. */
 replacing_renames=0;
 CHECK(tool("write",src,target)==0);CHECK(is(target,"new"));CHECK(no_leftovers());CHECK(!replacing_renames);
 /* A stale .old beside a complete file is removed first. */
 put(older,"stale");put(src,"newest");
 CHECK(tool("write",src,target)==0);CHECK(is(target,"newest"));CHECK(no_leftovers());
 /* Keep the current file when it cannot be moved aside. */
 fail_move_aside=1;put(src,"replaced");
 CHECK(tool("write",src,target)==8);CHECK(is(target,"newest"));CHECK(!replacing_renames);fail_move_aside=0;
 /* A stale backup cannot be cleared: neither file may be replaced. */
 put(older,"stale");fail_unlink=older;
 CHECK(tool("write",src,target)==8);CHECK(is(target,"newest")&&is(older,"stale")&&!replacing_renames);fail_unlink=0;
 /* A transient failure to open the current file is not proof it is gone. */
 fail_open=target;put(out,"untouched");
 CHECK(tool("load",target,out)==4);CHECK(is(out,"untouched"));
 CHECK(tool("write",src,target)==8);CHECK(is(target,"newest")&&is(older,"stale")&&!replacing_renames);
 fail_open=0;unlink(older);
 /* Interrupted between the moves: no target, .old and .new. load takes
  * .old, the previous complete file; read never does. */
 unlink(target);put(older,"old");put(newer,"partial?");
 CHECK(tool("load",target,out)==0);CHECK(is(out,"old"));
 CHECK(tool("read",target,out)==4); /* not absent either */
 extra_entry="UB820_~1.CFG";CHECK(tool("load",target,out)==4);CHECK(tool("write",src,target)==8);extra_entry=0;
 fail_readdir_after=1;CHECK(tool("load",target,out)==4);CHECK(tool("write",src,target)==8);fail_readdir_after=-1;
 put(tp0,"parked");CHECK(tool("load",target,out)==4);CHECK(tool("write",src,target)==8);unlink(tp0);
 CHECK(!exists(target)&&is(older,"old")&&!replacing_renames);
 /* The next write puts the new file in place and drops .old. */
 put(src,"next");CHECK(tool("write",src,target)==0);CHECK(is(target,"next"));CHECK(no_leftovers());
 /* Target and leftover .old (between the last move and the delete): target. */
 put(older,"older");CHECK(tool("load",target,out)==0);CHECK(is(out,"next"));unlink(older);
 /* An open error is not absence: the listing shows the file. */
 fail_open=target;CHECK(tool("load",target,out)==4);CHECK(tool("read",target,out)==4);fail_open=0;
 /* No target, no copy: absent only after a complete listing. */
 unlink(target);put(newer,"partial");
 CHECK(tool("load",target,out)==12); /* a .new alone was never complete */
 unlink(newer);CHECK(tool("load",target,out)==12);CHECK(tool("read",target,out)==12);
 fail_opendir=1;CHECK(tool("load",target,out)==4);fail_opendir=0;
 put(src,"x");put(out,"x");fail_readdir_after=1;CHECK(tool("load",target,out)==4);fail_readdir_after=-1;
 /* A copy DFAT parked (its own name, which cannot be tied to one target):
  * never read, never taken for absence. */
 put(tp0,"parked");CHECK(tool("load",target,out)==4);CHECK(tool("read",target,out)==4);unlink(tp0);
 {char tp2[160];snprintf(tp2,sizeof tp2,"%s/UB820_MENU.TP2",dir);put(tp2,"parked");CHECK(tool("load",target,out)==4);unlink(tp2);}
 {char log[160];snprintf(log,sizeof log,"%s/ub820_submenu.prev.tp0",dir);put(log,"other");CHECK(tool("load",target,out)==12);unlink(log);}
 /* An 8.3 alias may stand for the long name or a copy of it: not absent. */
 extra_entry="UB820_~1.CFG";CHECK(tool("load",target,out)==4);
 extra_entry="UB820_~2.TP0";CHECK(tool("load",target,out)==4);
 extra_entry="UB820_~1.OLD";CHECK(tool("load",target,out)==4);
 extra_entry="UB820_~1.NEW";CHECK(tool("load",target,out)==12);
 /* Only a real numeric-tail alias: a prefix of the stem, or Windows' hashed
  * form (1-2 stem characters, up to 4 hex digits), then ~1..~999999. */
 extra_entry="UB820~12.CFG";CHECK(tool("load",target,out)==4);
 extra_entry="UB~1.CFG";CHECK(tool("load",target,out)==4);
 extra_entry="UB2F1A~5.CFG";CHECK(tool("load",target,out)==4);
 extra_entry="U2F1A~1.CFG";CHECK(tool("load",target,out)==4);
 extra_entry="UB820_~1.TP3";CHECK(tool("load",target,out)==4);
 extra_entry="UB82F~1.OLD";CHECK(tool("load",target,out)==4);
 extra_entry="USER~1.CFG";CHECK(tool("load",target,out)==12);CHECK(tool("write",src,target)==0);unlink(target);
 extra_entry="u~backup.cfg";CHECK(tool("load",target,out)==12);
 extra_entry="UBX~1.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_~0.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_~12.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB2F1A9~1.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_~1X.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_~1.LOG";CHECK(tool("load",target,out)==12);
 extra_entry="~1.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_ME.CFG";CHECK(tool("load",target,out)==4);CHECK(tool("write",src,target)==8);
 extra_entry="UB820_ME.TP0";CHECK(tool("load",target,out)==4);CHECK(tool("write",src,target)==8);
 extra_entry="UB820_ME.OLD";CHECK(tool("load",target,out)==4);
 extra_entry="UB820_MX.CFG";CHECK(tool("load",target,out)==12);
 extra_entry="UB820_MENU.CFG";CHECK(tool("load",target,out)==4); /* case-insensitive */
 extra_entry=0;
 /* A file the helper only reads: a stray copy beside it is never used. */
 {char stray[180];snprintf(stray,sizeof stray,"%s.old",other);put(stray,"[ssh]\nenabled=1\n");
  CHECK(tool("read",other,out)==4);CHECK(!is(out,"[ssh]\nenabled=1\n"));unlink(stray);}
 CHECK(tool("read",other,out)==12);
 /* A new, empty stick: written directly. */
 put(src,"first");CHECK(tool("write",src,target)==0);CHECK(is(target,"first"));CHECK(no_leftovers());
 {char cmd[96];snprintf(cmd,sizeof cmd,"rm -rf %s",dir);if(system(cmd)){}}
 printf("ufs_replace_test %s (%d failures)\n",failures?"FAILED":"passed",failures);return failures!=0;
}
