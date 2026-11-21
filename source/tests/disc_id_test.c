/* core/disc_id.c on real directory trees: root discovery (/mnt/odd, then
 * UDF/ISO 9660 mounts), identifier stability, box-set discs with identical
 * index/MovieObject, explicit section boundaries, absent parts, limits. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
static char odd_path[256],mounts_path[256];
#define UB_DISC_ODD odd_path
#define UB_DISC_MOUNTS mounts_path
#include "../core/disc_id.c"

static char base[128];
static unsigned cases;
static void put(const char *root,const char *rel,const char *data,size_t n){
 char path[512];snprintf(path,sizeof path,"%s/%s",root,rel);
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
 assert(write(fd,data,n)==(ssize_t)n);close(fd);
}
static void dirs(const char *root){
 char p[512];mkdir(root,0700);
 snprintf(p,sizeof p,"%s/BDMV",root);mkdir(p,0700);
 snprintf(p,sizeof p,"%s/BDMV/PLAYLIST",root);mkdir(p,0700);
 snprintf(p,sizeof p,"%s/BDMV/STREAM",root);mkdir(p,0700);
}
/* A disc whose stub index/MovieObject match its box-set siblings. */
static void disc(const char *root,const char *const *playlists,const char *const *streams,size_t stream_size){
 char rel[128],*fill=calloc(1,stream_size+1);assert(fill);
 dirs(root);
 put(root,"BDMV/index.bdmv","INDX0200 stub",13);put(root,"BDMV/MovieObject.bdmv","MOBJ0200 stub",13);
 for(;*playlists;playlists++){snprintf(rel,sizeof rel,"BDMV/PLAYLIST/%s",*playlists);put(root,rel,"MPLS",4);}
 for(;*streams;streams++){snprintf(rel,sizeof rel,"BDMV/STREAM/%s",*streams);put(root,rel,fill,stream_size);}
 free(fill);
}
static void id_of(const char *root,char id[33]){assert(!ub_disc_id(root,id));assert(strlen(id)==32);}
static void rm_tree(const char *path){char cmd[600];snprintf(cmd,sizeof cmd,"rm -rf '%s'",path);assert(!system(cmd));}

int main(void){
 char a[256],b[256],c[256],id1[33],id2[33],id3[33],found[256];
 static const char *const pl[]={"00000.mpls","00001.mpls",0},*const pl2[]={"00000.mpls","00002.mpls",0};
 static const char *const st[]={"00000.m2ts","00001.m2ts",0},*const st_rev[]={"00001.m2ts","00000.m2ts",0};
 snprintf(base,sizeof base,"/tmp/ub820-disc-id.XXXXXX");assert(mkdtemp(base));
 snprintf(odd_path,sizeof odd_path,"%s/odd",base);snprintf(mounts_path,sizeof mounts_path,"%s/mounts",base);
 /* SHA-256 known answer (the shared header the injector also uses). */
 {struct ub_sha256 h;char out[65];ub_sha256_init(&h);ub_sha256_feed(&h,"ab",2);ub_sha256_feed(&h,"c",1);ub_sha256_end(&h,out);
  assert(!strcmp(out,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));cases++;}
 /* No disc anywhere. */
 mkdir(odd_path,0700);put(base,"mounts","",0);
 assert(!ub_disc_root(found,sizeof found));cases++;
 /* /mnt/odd first. */
 disc(odd_path,pl,st,10);
 snprintf(c,sizeof c,"%s/udf",base);disc(c,pl,st,10);
 {char m[600];int n=snprintf(m,sizeof m,"/dev/sr0 %s udf ro 0 0\n",c);put(base,"mounts",m,(size_t)n);}
 assert(ub_disc_root(found,sizeof found)&&!strcmp(found,odd_path));cases++;
 /* Otherwise the first UDF/ISO 9660 mount with BDMV/index.bdmv; other types are ignored. */
 rm_tree(odd_path);mkdir(odd_path,0700);
 snprintf(a,sizeof a,"%s/vfat",base);disc(a,pl,st,10);
 {char m[1200];int n=snprintf(m,sizeof m,"/dev/sda1 %s vfat rw 0 0\n/dev/sr1 %s/none iso9660 ro 0 0\n/dev/sr0 %s udf ro 0 0\n",a,base,c);
  put(base,"mounts",m,(size_t)n);}
 assert(ub_disc_root(found,sizeof found)&&!strcmp(found,c));cases++;
 assert(!ub_disc_root(found,4));cases++; /* path does not fit: no root */
 /* Stable and independent of directory order. */
 snprintf(a,sizeof a,"%s/a",base);snprintf(b,sizeof b,"%s/b",base);
 disc(a,pl,st,10);disc(b,pl,st_rev,10);
 id_of(a,id1);id_of(a,id2);assert(!strcmp(id1,id2));
 id_of(b,id3);assert(!strcmp(id1,id3));cases++;
 for(const char *p=id1;*p;p++)assert((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'));
 /* Box set: identical index/MovieObject, different stream sizes or playlists. */
 rm_tree(b);disc(b,pl,st,11);id_of(b,id2);assert(strcmp(id1,id2));
 rm_tree(b);disc(b,pl2,st,10);id_of(b,id2);assert(strcmp(id1,id2));cases++;
 /* Section boundaries are explicit: bytes moved between files change the id. */
 rm_tree(b);disc(b,pl,st,10);put(b,"BDMV/index.bdmv","ab",2);put(b,"BDMV/MovieObject.bdmv","c",1);id_of(b,id2);
 put(b,"BDMV/index.bdmv","a",1);put(b,"BDMV/MovieObject.bdmv","bc",2);id_of(b,id3);assert(strcmp(id2,id3));
 /* A file name moved between PLAYLIST and STREAM too. */
 rm_tree(b);{static const char *const pl3[]={"00000.mpls","00001.mpls","x",0};disc(b,pl3,st,4);}id_of(b,id2);
 rm_tree(b);{static const char *const st3[]={"00000.m2ts","00001.m2ts","x",0};disc(b,pl,st3,4);}id_of(b,id3);
 assert(strcmp(id2,id3));cases++;
 /* Absent MovieObject differs from an empty one; absent STREAM from an empty one. */
 rm_tree(b);disc(b,pl,st,10);put(b,"BDMV/MovieObject.bdmv","",0);id_of(b,id2);
 {char p[512];snprintf(p,sizeof p,"%s/BDMV/MovieObject.bdmv",b);assert(!unlink(p));}id_of(b,id3);assert(strcmp(id2,id3));
 rm_tree(b);{static const char *const none[]={0};disc(b,pl,none,1);}id_of(b,id2);
 {char p[512];snprintf(p,sizeof p,"%s/BDMV/STREAM",b);assert(!rmdir(p));}id_of(b,id3);assert(strcmp(id2,id3));cases++;
 /* No index.bdmv, or index.bdmv not a regular file: no identifier. */
 {char p[512];snprintf(p,sizeof p,"%s/BDMV/index.bdmv",b);assert(!unlink(p));assert(ub_disc_id(b,id2)==-1);
  assert(!mkdir(p,0700));assert(ub_disc_id(b,id2)==-1);assert(!rmdir(p));}
 assert(ub_disc_id("/nonexistent-ub820-disc",id2)==-1);cases++;
 /* A file over 1 MiB is not hashed. */
 rm_tree(b);disc(b,pl,st,10);
 {char *big=calloc(1,UB_DISC_MAX_FILE+1);assert(big);put(b,"BDMV/MovieObject.bdmv",big,UB_DISC_MAX_FILE);id_of(b,id2);
  put(b,"BDMV/MovieObject.bdmv",big,UB_DISC_MAX_FILE+1);assert(ub_disc_id(b,id2)==-1);free(big);}cases++;
 /* 4096 entries per directory are accepted, 4097 refused. */
 rm_tree(b);{static const char *const none[]={0};disc(b,pl,none,1);}
 for(unsigned i=0;i<UB_DISC_MAX_ENTRIES;i++){char rel[64];snprintf(rel,sizeof rel,"BDMV/STREAM/%05u.m2ts",i);put(b,rel,"",0);}
 id_of(b,id2);
 put(b,"BDMV/STREAM/extra.m2ts","",0);assert(ub_disc_id(b,id2)==-1);cases++;
 /* A subdirectory (STREAM/SSIF) counts as an entry with its type. */
 rm_tree(b);disc(b,pl,st,10);id_of(b,id2);
 {char p[512];snprintf(p,sizeof p,"%s/BDMV/STREAM/SSIF",b);assert(!mkdir(p,0700));}id_of(b,id3);assert(strcmp(id2,id3));cases++;
 rm_tree(base);
 printf("disc_id_test passed: %u cases; odd/UDF root, stable sorted id, box-set discs differ, tagged sections, absent parts, 1 MiB/4096 limits\n",cases);
 return 0;
}
