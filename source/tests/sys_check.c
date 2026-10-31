/* ARM check, run under qemu-arm -strace by run_tests.py: the helper's core
 * and all modules (diagnostics and continuous saving on) run a few ticks,
 * and the trace must not show a syscall the player's Linux 4.4 lacks
 * (statx, clock_gettime64, clock_nanosleep_time64, ...; see core/sys.h).
 * Also checks that the raw stat64 and getdents64 results are right. */
#include "ub.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)

int main(void){
 struct ub_stat st;struct ub_dir d;const char *name;int self=0,fd;long long size;unsigned long long t0;
 static const char cfg[]="[log]\nlevel=debug\nsave=always\ndiagnostics=1\n[ssh]\nenabled=0\n";
 fd=open(UB_CONFIG_DIR "/modules.cfg",O_WRONLY|O_CREAT|O_TRUNC,0600);
 if(fd<0||write(fd,cfg,sizeof cfg-1)!=(ssize_t)(sizeof cfg-1))return 2;
 close(fd);
 setvbuf(stdout,0,_IONBF,0); /* a buffered stdout would fstat (statx) */
 /* raw stat64 against lseek (not /proc/self/exe: qemu remaps it on open) */
 fd=open(UB_CONFIG_DIR "/modules.cfg",O_RDONLY);size=lseek(fd,0,SEEK_END);
 CHECK(size==(long long)sizeof cfg-1);
 CHECK(!ub_fstat(fd,&st));CHECK(st.size==size);CHECK((st.mode&0170000)==0100000);CHECK(st.ino&&st.mtime>1000000000);
 close(fd);
 CHECK(!ub_stat(UB_CONFIG_DIR "/modules.cfg",&st));CHECK(st.size==size);
 CHECK(!ub_stat(UB_CONFIG_DIR,&st));CHECK((st.mode&0170000)==0040000);
 CHECK(ub_stat("/nonexistent",&st));
 CHECK(!ub_dir_open(&d,"/proc"));
 while((name=ub_dir_next(&d)))if(!strcmp(name,"self"))self=1;
 ub_dir_close(&d);CHECK(self);
 t0=ub_now_ms();ub_sleep_ms(50);CHECK(ub_now_ms()-t0>=50&&ub_now_ms()-t0<1000);
 ub_log_open();
 ub_core_start();
 for(int i=0;i<3;i++){ub_core_tick();ub_sleep_ms(1000);}
 printf("%s (%d failures)\n",failures?"sys_check FAILED":"sys_check passed",failures);
 return failures!=0;
}
