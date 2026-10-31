#define _GNU_SOURCE
#include "log.h"
#include "sys.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UB_LOG_LIMIT (256*1024)
int ub_log_level=UB_INFO;

/* stderr becomes the RAM log so stray library messages land there too. */
void ub_log_open(void){
 int fd=open(UB_LOG_FILE,O_WRONLY|O_CREAT|O_APPEND,0600);
 if(fd>=0){dup2(fd,2);close(fd);}
 setvbuf(stderr,0,_IONBF,0);
}
/* Over the RAM budget: keep the newest half (from a line start), so a log
 * saved continuously still shows what led up to the present. */
static void trim(off_t size){
 static char keep[UB_LOG_LIMIT/2];
 static const char note[]="log trimmed to the newest 128 KB\n";
 ssize_t n;char *p;int fd=open(UB_LOG_FILE,O_RDONLY);
 if(fd<0)return;
 n=pread(fd,keep,sizeof keep,size-(off_t)sizeof keep);close(fd);
 if(n<=0||!(p=memchr(keep,'\n',(size_t)n))||ftruncate(2,0))return;
 p++;
 if(write(2,note,sizeof note-1)<0||write(2,p,(size_t)(keep+n-p))<0){}
}
void ub_log(const char *module,int level,const char *format,...){
 char line[512];struct timespec now;off_t size;va_list args;int n,m;
 if(level>ub_log_level&&level!=UB_ALWAYS)return;
 ub_now(&now);
 n=snprintf(line,sizeof line,"%5ld.%01ld %s: ",(long)now.tv_sec,(long)(now.tv_nsec/100000000),module?module:"core");
 if(n<0||n>=(int)sizeof line)return;
 va_start(args,format);m=vsnprintf(line+n,sizeof line-(size_t)n,format,args);va_end(args);
 if(m<0)return;
 n+=m;if(n>(int)sizeof line-2)n=(int)sizeof line-2;
 while(n>0&&line[n-1]=='\n')n--;
 line[n++]='\n';
 size=lseek(2,0,SEEK_END); /* fd 2 appends; no fstat (see sys.h) */
 if(size>UB_LOG_LIMIT)trim(size);
 if(write(2,line,(size_t)n)<0){}
}
