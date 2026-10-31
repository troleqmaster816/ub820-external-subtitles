#define _GNU_SOURCE
#include "ub.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int mem_io(int pid,uint32_t address,void *buffer,size_t length,int writing){
 char path[32];int fd;ssize_t n;
 snprintf(path,sizeof path,"/proc/%d/mem",pid);
 fd=open(path,writing?O_RDWR:O_RDONLY);if(fd<0)return -1;
 n=writing?pwrite(fd,buffer,length,(off_t)address):pread(fd,buffer,length,(off_t)address);
 close(fd);return n==(ssize_t)length?0:-1;
}
int ub_mem_read(int pid,uint32_t address,void *buffer,size_t length){return mem_io(pid,address,buffer,length,0);}
int ub_mem_write(int pid,uint32_t address,const void *buffer,size_t length){return mem_io(pid,address,(void *)buffer,length,1);}

/* Whole /proc files are read with ub_read_file rather than fopen (sys.h). */
uint32_t ub_map_base(int pid,const char *suffix,char *path,size_t path_size){
 static char text[256*1024];
 char p[64],name[600],perm[8],*line,*save;unsigned long start,end,off;uint32_t base=0;size_t n,k=strlen(suffix);
 snprintf(p,sizeof p,"/proc/%d/maps",pid);if(ub_read_file(p,text,sizeof text)<0)return 0;
 for(line=strtok_r(text,"\n",&save);!base&&line;line=strtok_r(0,"\n",&save)){
  name[0]=0;
  if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %599s",&start,&end,perm,&off,name)!=5||off||name[0]!='/')continue;
  n=strlen(name);if(n<k||strcmp(name+n-k,suffix))continue;
  if(path){if(n>=path_size)continue;memcpy(path,name,n+1);}
  base=(uint32_t)start;
 }
 return base;
}

int ub_mount_for(const char *path,char *line,size_t line_size){
 char text[8192],dev[200],dir[200],type[40],opts[200],*buf,*save;size_t best=0;
 if(ub_read_file("/proc/mounts",text,sizeof text)<0)return -1;
 for(buf=strtok_r(text,"\n",&save);buf;buf=strtok_r(0,"\n",&save)){
  size_t n;
  if(sscanf(buf,"%199s %199s %39s %199s",dev,dir,type,opts)!=4)continue;
  n=strlen(dir);
  if(strncmp(path,dir,n)||(n>1&&path[n]&&path[n]!='/'))continue;
  if(n>best){best=n;snprintf(line,line_size,"%s on %s type %s (%s)",dev,dir,type,opts);}
 }
 return best?0:-1;
}
