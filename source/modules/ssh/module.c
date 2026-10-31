/* On-demand diagnostic SSH server (Dropbear), started from the OPTIONS menu.
 *
 * Off by default. When the user picks "Start SSH", this module:
 *   - writes the embedded static dropbear/dropbearkey to /tmp,
 *   - copies the chosen secret from the stick (sub/authorized_keys for key auth,
 *     or sub/ssh_password for password auth) through the player's file layer,
 *   - for password auth, turns the plaintext into a $6$ hash (sha512crypt.h) so
 *     only the hash lives in RAM,
 *   - generates an ephemeral host key,
 *   - mounts devpts (best effort, for interactive shells) and launches dropbear
 *     in the foreground as its own child, so dropbear's messages reach the log.
 * Programs are started with default signal handling, nothing blocked and only
 * stdin/stdout/stderr open (clean_exec), so nothing the helper inherited from
 * the player process it was started from reaches ssh sessions.
 * Everything is in RAM; a power cycle removes it. Dropbear is patched to read
 * authorized_keys and the password hash from fixed /tmp paths (see
 * tools/ssh/patch_dropbear.py), because /root is read-only and there is no
 * /etc/shadow on this device. */
#define _GNU_SOURCE
#include "ub.h"
#include "ssh.h"
#include "storage.h"
#include "tools.h"
#include "sha512crypt.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#define ID "ssh"
#define DIR "/tmp/.ub820_ssh"
#define DROPBEAR_BIN "/tmp/.ub820_dropbear"
#define DROPBEARKEY_BIN "/tmp/.ub820_dropbearkey"
#define HOSTKEY DIR "/hostkey"
#define AUTHKEYS DIR "/authorized_keys"
#define PWHASH DIR "/pwhash"
#define PIDFILE DIR "/pid"
#define STICK_AUTHKEYS UB_STICK_DIR "/authorized_keys"
#define STICK_PASSWORD UB_STICK_DIR "/ssh_password"

enum { AUTH_KEY=0, AUTH_PASSWORD=1 };
static int auth=AUTH_KEY, port=2222, guard=0;
static const char *const auth_names[]={"key","password",0};
static const struct ub_option options[]={
 {"auth",0,1,&auth,auth_names},
 {"port",1024,65535,&port,0},
 {"guard",0,1,&guard,0},
 {0,0,0,0,0}
};

#include "session_guard.h"

/* Child side of every program started here. The helper is started from inside
 * a player process: its signal mask and ignored signals came from there and
 * survive exec, and it holds files such as its single-instance lock. None of
 * that may reach dropbear and, through it, every ssh session. */
__attribute__((noreturn)) static void clean_exec(char *const argv[],int keep_stderr){
 static struct ub_dir d;const char *e;int fds[256],n=0,nul=open("/dev/null",O_RDWR);sigset_t none;
 for(int sig=1;sig<65;sig++)signal(sig,SIG_DFL); /* SIGKILL/SIGSTOP and libc's own just fail */
 sigemptyset(&none);sigprocmask(SIG_SETMASK,&none,0);
 if(nul>=0){dup2(nul,0);dup2(nul,1);if(!keep_stderr)dup2(nul,2);}
 if(!ub_dir_open(&d,"/proc/self/fd")){
  while(n<256&&(e=ub_dir_next(&d)))if(e[0]>='0'&&e[0]<='9'&&atoi(e)>2&&atoi(e)!=d.fd)fds[n++]=atoi(e);
  ub_dir_close(&d);
  for(int i=0;i<n;i++)close(fds[i]);
 }else for(int fd=3;fd<1024;fd++)close(fd);
 execv(argv[0],argv);_exit(127);
}

static pid_t guard_pid;
static void guard_start(pid_t master){
 pid_t pid;
 if(guard_pid>0||master<=0)return;
 pid=fork();
 if(!pid)guard_loop(master);
 if(pid>0)guard_pid=pid;
}
static void guard_stop(void){if(guard_pid>0){kill(guard_pid,SIGKILL);waitpid(guard_pid,0,0);}guard_pid=0;}

static int write_blob(const char *path,const unsigned char *data,size_t size,int mode){
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,mode);size_t done=0;ssize_t n;
 if(fd<0)return -1;
 while(done<size&&(n=write(fd,data+done,size-done))>0)done+=(size_t)n;
 if(close(fd)||done!=size){unlink(path);return -1;}
 return 0;
}
/* Run argv to completion (<=timeout s). Returns exit code, or -1. */
static int run(char *const argv[],int timeout){
 pid_t pid=fork();int status,i;
 if(pid<0)return -1;
 if(!pid)clean_exec(argv,0);
 for(i=0;i<timeout*10;i++){pid_t r=waitpid(pid,&status,WNOHANG);if(r==pid)return WIFEXITED(status)?WEXITSTATUS(status):-1;ub_sleep_ms(100);}
 kill(pid,SIGKILL);waitpid(pid,&status,0);return -1;
}
/* LAN address, found by the kernel's own route choice; no packet is sent. */
static void local_ip(char *out,size_t n){
 int s=socket(AF_INET,SOCK_DGRAM,0);struct sockaddr_in a;socklen_t al=sizeof a;
 snprintf(out,n,"the player's IP");
 if(s<0)return;
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(53);a.sin_addr.s_addr=inet_addr("8.8.8.8");
 if(connect(s,(struct sockaddr *)&a,sizeof a)==0&&getsockname(s,(struct sockaddr *)&a,&al)==0)
  snprintf(out,n,"%s",inet_ntoa(a.sin_addr));
 close(s);
}
/* dropbear runs in the foreground as this helper's child. A PID is trusted
 * only while it still runs our dropbear binary: after dropbear ends, its
 * number may belong to a player process, which must never be signalled. */
static pid_t dropbear_pid;
static int is_our_dropbear(pid_t pid){
 char path[40],exe[96];ssize_t n;
 snprintf(path,sizeof path,"/proc/%d/exe",(int)pid);
 n=readlink(path,exe,sizeof exe-1);if(n<=0)return 0;
 exe[n]=0;return !strncmp(exe,DROPBEAR_BIN,strlen(DROPBEAR_BIN))&&(!exe[strlen(DROPBEAR_BIN)]||exe[strlen(DROPBEAR_BIN)]==' ');
}
static pid_t running_pid(void){
 int status;
 if(dropbear_pid<=0)return 0;
 if(waitpid(dropbear_pid,&status,WNOHANG)==dropbear_pid){
  if(WIFSIGNALED(status))ub_log(ID,UB_ALWAYS,"dropbear PID%d ended by signal %d",(int)dropbear_pid,WTERMSIG(status));
  else ub_log(ID,UB_ALWAYS,"dropbear PID%d ended with exit code %d",(int)dropbear_pid,WEXITSTATUS(status));
  dropbear_pid=0;unlink(PIDFILE);return 0;
 }
 return is_our_dropbear(dropbear_pid)?dropbear_pid:0;
}

/* status for the menu: "<running> <code> <text>" */
static void answer(int running,int code,const char *text){
 char line[200];int n=snprintf(line,sizeof line,"%d %d %s\n",running,code,text),fd;
 if(n<0||n>=(int)sizeof line)return;
 fd=open(UB_SSH_STATUS ".tmp",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return;
 if(write(fd,line,(size_t)n)!=n){close(fd);unlink(UB_SSH_STATUS ".tmp");return;}
 if(close(fd)||rename(UB_SSH_STATUS ".tmp",UB_SSH_STATUS))unlink(UB_SSH_STATUS ".tmp");
}

static int prepare_secret(char *err,size_t errn){
 char tmp[64];int rc;
 if(auth==AUTH_KEY){
  rc=ub_stick_read(STICK_AUTHKEYS,AUTHKEYS);
  if(rc){snprintf(err,errn,"no sub/authorized_keys on the stick (%s)",ub_stick_error(rc));return -1;}
  chmod(AUTHKEYS,0600);
  return 0;
 }
 /* password: copy plaintext, hash it, keep only the hash */
 snprintf(tmp,sizeof tmp,"%s/pw.plain",DIR);
 rc=ub_stick_read(STICK_PASSWORD,tmp);
 if(rc){snprintf(err,errn,"no sub/ssh_password on the stick (%s)",ub_stick_error(rc));return -1;}
 {
  char plain[128],hash[128],salt[17];int fd=open(tmp,O_RDONLY);ssize_t n;unsigned i;
  n=fd>=0?read(fd,plain,sizeof plain-1):-1;if(fd>=0)close(fd);unlink(tmp);
  if(n<=0){snprintf(err,errn,"sub/ssh_password is empty");return -1;}
  plain[n]=0;plain[strcspn(plain,"\r\n")]=0;
  if(!plain[0]){snprintf(err,errn,"sub/ssh_password is empty");return -1;}
  {
   static const char set[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
   unsigned char r[16];int rf=open("/dev/urandom",O_RDONLY);
   if(rf<0||read(rf,r,16)!=16){if(rf>=0)close(rf);snprintf(err,errn,"no randomness for the salt");return -1;}
   close(rf);for(i=0;i<16;i++)salt[i]=set[r[i]&63];salt[16]=0;
  }
  ub_sha512_crypt(plain,salt,hash);
  memset(plain,0,sizeof plain);
  if(write_blob(PWHASH,(unsigned char *)hash,strlen(hash),0600)){snprintf(err,errn,"cannot write the password hash");return -1;}
 }
 return 0;
}

static int start_ssh(char *msg,size_t msgn){
 char ip[64],portstr[8],err[160];
 if(running_pid()){local_ip(ip,sizeof ip);snprintf(msg,msgn,"already running: ssh root@%s -p %d",ip,port);return 0;}
 if(mkdir(DIR,0700)&&errno!=EEXIST){snprintf(msg,msgn,"cannot create %s",DIR);return -1;}
 chmod(DIR,0700);
 unlink(AUTHKEYS);unlink(PWHASH);
 if(write_blob(DROPBEAR_BIN,ub_tool_dropbear,ub_tool_dropbear_size,0700)||
    write_blob(DROPBEARKEY_BIN,ub_tool_dropbearkey,ub_tool_dropbearkey_size,0700)){
  snprintf(msg,msgn,"cannot write the ssh binaries to /tmp");return -1;
 }
 if(prepare_secret(err,sizeof err)){snprintf(msg,msgn,"%s",err);return -1;}
 unlink(HOSTKEY);
 {
  char *kv[]={(char *)DROPBEARKEY_BIN,(char *)"-t",(char *)"ed25519",(char *)"-f",(char *)HOSTKEY,0};
  if(run(kv,20)){snprintf(msg,msgn,"host key generation failed");return -1;}
 }
 /* best effort: interactive shells need a pty slave */
 if(mount("none","/dev/pts","devpts",0,0)&&errno!=EBUSY)
  ub_log(ID,UB_INFO,"could not mount devpts (%s); non-interactive ssh still works",strerror(errno));
 snprintf(portstr,sizeof portstr,"%d",port);
 /* -F: dropbear stays in the foreground as our child, its stderr is the log
  * (connections, logins, its own errors). Key mode adds -s (no passwords);
  * password mode leaves password auth on (there is no authorized_keys). */
 {
  char *a[]={(char *)DROPBEAR_BIN,(char *)"-F",(char *)"-r",(char *)HOSTKEY,(char *)"-p",portstr,(char *)"-P",(char *)PIDFILE,
             auth==AUTH_KEY?(char *)"-s":0,0};
  pid_t pid=fork();
  if(pid<0){snprintf(msg,msgn,"cannot start dropbear");return -1;}
  if(!pid)clean_exec(a,1);
  dropbear_pid=pid;
 }
 ub_sleep_ms(1000);
 if(!running_pid()){snprintf(msg,msgn,"dropbear exited at startup; see the log");return -1;}
 if(guard)guard_start(running_pid());
 local_ip(ip,sizeof ip);
 snprintf(msg,msgn,"ssh root@%s -p %d (%s auth)",ip,port,auth==AUTH_KEY?"key":"password");
 return 0;
}
static void stop_ssh(char *msg,size_t msgn){
 pid_t p=running_pid();unsigned sessions;
 guard_stop();
 if(!p){snprintf(msg,msgn,"not running");return;}
 sessions=tree_end(p,0); /* open sessions and their shells end with the server */
 kill(p,SIGTERM);
 for(int i=0;i<10&&running_pid();i++)ub_sleep_ms(300);
 if(running_pid()){kill(p,SIGKILL);waitpid(p,0,0);dropbear_pid=0;}
 unlink(PIDFILE);unlink(PWHASH);unlink(AUTHKEYS);
 if(sessions)snprintf(msg,msgn,"stopped (and %u session processes)",sessions);
 else snprintf(msg,msgn,"stopped");
}

static void step(struct ub_part *p,struct ub_target *t){
 char buf[32],msg[200];int fd;ssize_t n;int want_stop;
 (void)p;(void)t;
 if(guard_pid>0&&waitpid(guard_pid,0,WNOHANG)==guard_pid)guard_pid=0;
 if(guard&&!guard_pid&&running_pid())guard_start(running_pid()); /* the server outlived its guard */
 fd=open(UB_SSH_REQUEST,O_RDONLY);if(fd<0)return;
 n=read(fd,buf,sizeof buf-1);close(fd);unlink(UB_SSH_REQUEST);
 if(n<=0)return;
 buf[n]=0;
 if(!strncmp(buf,"stop",4))want_stop=1;
 else if(!strncmp(buf,"start",5))want_stop=0;
 else if(!strncmp(buf,"toggle",6))want_stop=running_pid()?1:0;
 else{ub_log(ID,UB_INFO,"ignored ssh request");return;}
 if(want_stop){stop_ssh(msg,sizeof msg);ub_log(ID,UB_ALWAYS,"ssh server: %s",msg);answer(running_pid()?1:0,0,msg);}
 else{
  int rc=start_ssh(msg,sizeof msg);
  if(rc)ub_log(ID,UB_ALWAYS,"ssh start failed: %s",msg);
  else ub_log(ID,UB_ALWAYS,"ssh server: %s",msg);
  answer(running_pid()?1:0,rc,msg);
 }
}

/* Marker so the menu shows the "Start / Stop SSH" row only when this module is
 * enabled (the core calls start() for enabled modules only). */
#define UB_SSH_ENABLED "/tmp/ub820_ssh.enabled"
static void start(struct ub_module *m){int fd;(void)m;fd=open(UB_SSH_ENABLED,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0)close(fd);}

static struct ub_part parts[]={
 {"server",0,0,0,0,0,step,{0},0,0},
};
struct ub_module ub_module_ssh={
 ID,"On-demand diagnostic SSH server (Dropbear); key or password auth",
 0,options,start,parts,sizeof parts/sizeof *parts,0
};
