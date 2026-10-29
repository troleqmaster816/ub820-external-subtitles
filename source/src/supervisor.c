/* Replaces only USB sub/engage. The owner-proven MKV loader is unchanged. */
#define main ub_engage_unused_main
#define style_patch legacy_style_patch
#define font_size_set legacy_font_size_set
#define playdat_set_on legacy_playdat_set_on
#include "engage_core.c"
#undef main
#undef style_patch
#undef font_size_set
#undef playdat_set_on
#include "disc_style.h"
#include "injector.h"
#include "embedded_payloads.h"
#include "protocol.h"
#include <time.h>
#include <sys/stat.h>

static int find_named(const char *name){
 char buf[4096] __attribute__((aligned(8))),p[64],comm[64];int found=0,n,fd=open("/proc",O_RDONLY|O_DIRECTORY);if(fd<0)return 0;
 while(!found&&(n=(int)syscall(SYS_getdents64,fd,buf,sizeof buf))>0){int off=0;while(off<n){struct linux_dirent64 *e=(void *)(buf+off);if(e->d_reclen<20||off+e->d_reclen>n)break;off+=e->d_reclen;int pid=atoi(e->d_name);if(pid<=0)continue;snprintf(p,sizeof p,"/proc/%d/comm",pid);int cfd=open(p,O_RDONLY);if(cfd<0)continue;ssize_t k=read(cfd,comm,sizeof comm-1);close(cfd);if(k<=0)continue;comm[k]=0;comm[strcspn(comm,"\r\n")]=0;if(!strcmp(comm,name)){found=pid;break;}}}
 close(fd);return found;
}
static unsigned long long birth(int pid){char p[64],buf[2048],*q,*save;int fd;ssize_t n;unsigned field=3;snprintf(p,sizeof p,"/proc/%d/stat",pid);fd=open(p,O_RDONLY);if(fd<0)return 0;n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return 0;buf[n]=0;q=strrchr(buf,')');if(!q)return 0;for(q=strtok_r(q+1," ",&save);q;q=strtok_r(0," ",&save),field++)if(field==22)return strtoull(q,0,10);return 0;}
struct watched {int pid,installed,failed,bootstrap;unsigned long long born;unsigned retry;};
static void track(struct watched *w,const struct ub_payload *p){
 int pid=find_named(p->process);unsigned long long t=pid?birth(pid):0;
 if(pid!=w->pid||t!=w->born){memset(w,0,sizeof *w);w->pid=pid;w->born=t;}
 if(!pid||!t||w->installed||w->failed)return;
 if(w->retry){w->retry--;return;}
 /* An obsolete status must not terminate the new process's bootstrap. The UI
  * notices its disappearance/epoch change and obtains a fresh LIST. */
 if(!strcmp(p->process,"apl_meda_bdplay")){unlink(UB_STATUS_FILE);unlink(UB_REQUEST_FILE);}
 int result=ub_install_payload(pid,p);
 if(result==0){w->installed=1;return;}
 if(result==-2){w->failed=1;return;}
 /* Bounded retries: a persistent incompatible platform fails closed and leaves
  * the original native files and instruction sites intact. */
 w->bootstrap++;w->retry=4;if(w->bootstrap>=12){w->failed=1;fprintf(stderr,"Giving up installing %s in PID%d; inspect log after power cycle.\n",p->process,pid);}
}
int main(int argc,char **argv){
 int lockfd;struct watched ui={0},bd={0};unsigned bootstrap_attempts=0;int bootpid=0;unsigned long long bootborn=0;
 if(argc==3&&!strcmp(argv[1],"--hash")){char h[65];if(ub_hash_file(argv[2],h))return 2;puts(h);return 0;}
 if(argc==2&&!strcmp(argv[1],"--selftest")){char h[65];if(ub_hash_file("/proc/self/exe",h))return 2;printf("ARM native supervisor self-test: SHA256=%s\n",h);return 0;}
 lockfd=open("/tmp/ub820_submenu.supervisor.lock",O_CREAT|O_RDWR,0600);if(lockfd<0||flock(lockfd,LOCK_EX|LOCK_NB))return 0;
 int logfd=open("/tmp/ub820_submenu.log",O_WRONLY|O_CREAT|O_APPEND,0600);if(logfd>=0){dup2(logfd,2);close(logfd);}setvbuf(stderr,0,_IONBF,0);
 fprintf(stderr,"UB820 external subtitle menu v1 disc-look starting (RAM only).\n");
 disc_style_load();
 for(;;){
  track(&ui,&ub_ui_payload);track(&bd,&ub_backend_payload);
  if(bd.pid!=bootpid||bd.born!=bootborn){bootpid=bd.pid;bootborn=bd.born;bootstrap_attempts=0;}
  /* Preserve the working native font/outline patch. Initial On is established
   * once per playback process; subsequent Off/On belongs to the custom menu. */
  if(bd.installed&&bootstrap_attempts<15){
   unsigned char loaded=0;
   struct ub_status status;int sfd=open(UB_STATUS_FILE,O_RDONLY);
   if(sfd>=0){ssize_t got=read(sfd,&status,sizeof status);close(sfd);if(got==(ssize_t)sizeof status&&status.magic==UB_MAGIC&&status.version==UB_VERSION){bootstrap_attempts=15;}}
   if(bootstrap_attempts>=15){sleep(1);continue;}
   if(mem_read(bd.pid,LOADED_ADDR,&loaded,1)==0){
    if(loaded){bootstrap_attempts=15;}
    else{
     if(bootstrap_attempts==0){font_size_set();if(style_patch(bd.pid))fprintf(stderr,"Style initialization incomplete\n");if(playdat_set_on())fprintf(stderr,"Initial subtitle play-data write failed\n");if(mem_write_u32(bd.pid,GATE_ADDR,1))fprintf(stderr,"Initial subtitle gate write failed\n");}
     if(evt_load())fprintf(stderr,"Initial subtitle load event failed\n");
     bootstrap_attempts++;
    }
   }
  }
  sleep(1);
 }
}
