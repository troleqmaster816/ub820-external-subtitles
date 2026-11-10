/* The real copy tool must not publish a file if vendor close failed. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int ufs_main(int,char **);
static int close_result,read_count;
int UFS_Init(void){return 0;}
int UFS_Open(const char *path,unsigned flags,unsigned mode,unsigned *h){
 (void)path;(void)flags;(void)mode;*h=7;read_count=0;return 0;
}
int UFS_Read(unsigned h,void *buf,unsigned long long n,unsigned long long *done){
 (void)h;(void)n;*done=read_count++?0:4;if(*done)memcpy(buf,"new\n",4);return 0;
}
int UFS_Write(unsigned h,void *buf,unsigned long long n,unsigned long long *done){
 (void)h;(void)buf;*done=n;return 0;
}
int UFS_Close(unsigned h){(void)h;return close_result;}
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL ufs_test:%d: %s\n",__LINE__,#c);failures++;}}while(0)
int main(void){
 char dir[]="/tmp/ub820-ufs-test-XXXXXX",path[160],temp[170],text[16]={0};int fd;ssize_t n;
 CHECK(mkdtemp(dir)!=0);snprintf(path,sizeof path,"%s/result",dir);snprintf(temp,sizeof temp,"%s.ufs",path);
 char *argv[]={"ufs","read","fixture-source",path,0};
 fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
 if(fd>=0){CHECK(write(fd,"old\n",4)==4);close(fd);}
 close_result=-123;CHECK(ufs_main(4,argv)==8);CHECK(access(temp,F_OK));
 fd=open(path,O_RDONLY);n=fd>=0?read(fd,text,sizeof text):0;if(fd>=0)close(fd);
 CHECK(n==4&&!memcmp(text,"old\n",4));
 close_result=0;CHECK(ufs_main(4,argv)==0);CHECK(access(temp,F_OK));
 fd=open(path,O_RDONLY);n=fd>=0?read(fd,text,sizeof text):0;if(fd>=0)close(fd);
 CHECK(n==4&&!memcmp(text,"new\n",4));
 unlink(path);CHECK(!rmdir(dir));
 printf("ufs_test %s (%d failures)\n",failures?"FAILED":"passed",failures);return failures!=0;
}
