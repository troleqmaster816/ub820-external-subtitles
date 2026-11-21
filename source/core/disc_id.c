/* Disc identifier for the subtitle memory (disc_id.h). Uses only the 4.4
 * syscalls in sys.c (stat64, getdents64), no statx/opendir. */
#include "disc_id.h"
#include "sys.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef UB_DISC_ODD
#define UB_DISC_ODD "/mnt/odd"
#endif
#ifndef UB_DISC_MOUNTS
#define UB_DISC_MOUNTS "/proc/mounts"
#endif
#define UB_DISC_MAX_ENTRIES 4096u
#define UB_DISC_MAX_FILE (1024u*1024u)

static int join(char *out,size_t cap,const char *a,const char *b,const char *c){
 int n=snprintf(out,cap,"%s%s%s",a,b,c);return n<0||(size_t)n>=cap?-1:0;
}
static int has_index(const char *root){
 char path[512];struct ub_stat st;
 return !join(path,sizeof path,root,"/BDMV/index.bdmv","")&&!ub_stat(path,&st)&&S_ISREG(st.mode);
}
int ub_disc_root(char *out,size_t size){
 static char text[16384];char *line,*save,dev[200],dir[200],type[40];
 if(!size)return 0;
 if(has_index(UB_DISC_ODD))return !join(out,size,UB_DISC_ODD,"","");
 if(ub_read_file(UB_DISC_MOUNTS,text,sizeof text)<0)return 0;
 for(line=strtok_r(text,"\n",&save);line;line=strtok_r(0,"\n",&save)){
  if(sscanf(line,"%199s %199s %39s",dev,dir,type)!=3||(strcmp(type,"udf")&&strcmp(type,"iso9660")))continue;
  if(has_index(dir))return !join(out,size,dir,"","");
 }
 return 0;
}

static void feed_u64(struct ub_sha256 *h,uint64_t v){
 unsigned char b[8];for(unsigned i=0;i<8;i++)b[i]=(unsigned char)(v>>(56-8*i));ub_sha256_feed(h,b,8);
}
static void feed_tag(struct ub_sha256 *h,const char *tag){ub_sha256_feed(h,tag,strlen(tag)+1);}
/* Tag, then UINT64_MAX when absent (allowed only for optional files), else
 * the length and the content. */
static int feed_file(struct ub_sha256 *h,const char *root,const char *rel,int required){
 char path[512];unsigned char buf[4096];struct ub_stat st;uint64_t done=0;int fd;
 if(join(path,sizeof path,root,"/",rel))return -1;
 feed_tag(h,rel);
 fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0){if(required||errno!=ENOENT)return -1;feed_u64(h,UINT64_MAX);return 0;}
 if(ub_fstat(fd,&st)||!S_ISREG(st.mode)||st.size<0||st.size>(long long)UB_DISC_MAX_FILE){close(fd);return -1;}
 feed_u64(h,(uint64_t)st.size);
 while(done<(uint64_t)st.size){
  size_t want=(uint64_t)st.size-done<sizeof buf?(size_t)((uint64_t)st.size-done):sizeof buf;
  ssize_t n=read(fd,buf,want);
  if(n<0&&errno==EINTR)continue;
  if(n<=0){close(fd);return -1;} /* shorter than its size: not a stable disc */
  ub_sha256_feed(h,buf,(size_t)n);done+=(uint64_t)n;
 }
 close(fd);return 0;
}
static const char *sort_pool;
static int name_order(const void *a,const void *b){
 return strcmp(sort_pool+*(const uint32_t *)a,sort_pool+*(const uint32_t *)b);
}
/* Tag, then UINT64_MAX when absent, else the count and, sorted by name, each
 * entry's name length, name, type ('f' file, 'd' directory, 'o' other) and
 * size (files only). */
static int feed_dir(struct ub_sha256 *h,const char *root,const char *rel){
 static struct ub_dir dir;char path[512],file[1024];const char *name;
 char *pool=0;uint32_t *offsets=0;size_t pool_used=0,pool_cap=0;unsigned count=0;int rc=-1,r;
 if(join(path,sizeof path,root,"/",rel))return -1;
 feed_tag(h,rel);
 if(ub_dir_open(&dir,path)){if(dir.err!=ENOENT)return -1;feed_u64(h,UINT64_MAX);return 0;}
 offsets=malloc(UB_DISC_MAX_ENTRIES*sizeof *offsets);
 if(!offsets)goto out;
 while((r=ub_dir_read(&dir,&name))==1){
  size_t n=strlen(name)+1;
  if(!strcmp(name,".")||!strcmp(name,".."))continue;
  if(count>=UB_DISC_MAX_ENTRIES)goto out;
  if(pool_used+n>pool_cap){
   size_t cap=pool_cap?pool_cap*2:4096;char *grown;
   while(cap<pool_used+n)cap*=2;
   if(!(grown=realloc(pool,cap)))goto out;
   pool=grown;pool_cap=cap;
  }
  memcpy(pool+pool_used,name,n);offsets[count++]=(uint32_t)pool_used;pool_used+=n;
 }
 if(r<0)goto out; /* an incomplete list is no identity */
 sort_pool=pool;
 if(count)qsort(offsets,count,sizeof *offsets,name_order);
 feed_u64(h,count);
 for(unsigned i=0;i<count;i++){
  const char *entry=pool+offsets[i];struct ub_stat st;unsigned char type;
  if(join(file,sizeof file,path,"/",entry)||ub_stat(file,&st))goto out;
  type=S_ISREG(st.mode)?'f':S_ISDIR(st.mode)?'d':'o';
  feed_u64(h,strlen(entry));ub_sha256_feed(h,entry,strlen(entry));ub_sha256_feed(h,&type,1);
  feed_u64(h,type=='f'&&st.size>0?(uint64_t)st.size:0);
 }
 rc=0;
 out:
 ub_dir_close(&dir);free(pool);free(offsets);sort_pool=0;
 return rc;
}
int ub_disc_id(const char *root,char id[33]){
 struct ub_sha256 h;char digest[65];
 ub_sha256_init(&h);
 feed_tag(&h,"UB820 disc id v1");
 if(feed_file(&h,root,"BDMV/index.bdmv",1)||feed_file(&h,root,"BDMV/MovieObject.bdmv",0)||
    feed_dir(&h,root,"BDMV/PLAYLIST")||feed_dir(&h,root,"BDMV/STREAM"))return -1;
 ub_sha256_end(&h,digest);
 memcpy(id,digest,32);id[32]=0;
 return 0;
}
