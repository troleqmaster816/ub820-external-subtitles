/* Actual menu client/server helpers: concurrent clients, stale acknowledgements,
 * monotonic timeout, and replacement of a legacy mailbox during its claim. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "helper_menu.h"
#include "ssh.h"
#include "storage.h"
#ifndef UB_ACTION_TEST_DIR
#define UB_ACTION_TEST_DIR "/tmp/ub820-action-test"
#endif
#undef UB_REGION_REQUEST
#undef UB_REGION_STATUS
#undef UB_DVD_REQUEST
#undef UB_DVD_STATUS
#undef UB_REMOTE_REQUEST
#undef UB_REMOTE_STATUS
#undef UB_SSH_REQUEST
#undef UB_SSH_STATUS
#undef UB_STORAGE_REQUEST
#undef UB_STORAGE_STATUS
#define UB_REGION_REQUEST UB_ACTION_TEST_DIR "/region.request"
#define UB_REGION_STATUS UB_ACTION_TEST_DIR "/region.status"
#define UB_DVD_REQUEST UB_ACTION_TEST_DIR "/dvd.request"
#define UB_DVD_STATUS UB_ACTION_TEST_DIR "/dvd.status"
#define UB_REMOTE_REQUEST UB_ACTION_TEST_DIR "/remote.request"
#define UB_REMOTE_STATUS UB_ACTION_TEST_DIR "/remote.status"
#define UB_SSH_REQUEST UB_ACTION_TEST_DIR "/ssh.request"
#define UB_SSH_STATUS UB_ACTION_TEST_DIR "/ssh.status"
#define UB_STORAGE_REQUEST UB_ACTION_TEST_DIR "/storage.request"
#define UB_STORAGE_STATUS UB_ACTION_TEST_DIR "/storage.status"
static uint32_t seconds=100;
static void test_server_now(struct timespec *out){out->tv_sec=seconds;out->tv_nsec=0;}
static int replace_legacy;
static void put(const char *path,const char *text){FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static int test_rename(const char *from,const char *to){
 int rc=rename(from,to);
 if(!rc&&replace_legacy&&!strcmp(from,UB_REMOTE_REQUEST)){replace_legacy=0;put(from,"set helper 2\n");}
 return rc;
}
#define rename test_rename
#define ub_now test_server_now
#include "action_ipc.h"
#undef ub_now
#undef rename
static int test_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);out->tv_sec=seconds;out->tv_nsec=0;return 0;}
#define clock_gettime test_clock
#include "../modules/ui/helper_actions.h"
#undef clock_gettime
static void reply_to(struct helper_state *s,const char *base,unsigned seq,const char *line){char path[UB_ACTION_PATH];assert(!ub_action_path(path,sizeof path,base,'r',s->action_session,seq));assert(!ub_action_reply(path,line,strlen(line)));}
int main(void){
 struct helper_state a={0},b={0},dead={0};char path[UB_ACTION_PATH],reply[UB_ACTION_PATH],buf[160];
 mkdir(UB_ACTION_TEST_DIR,0700);
 put(UB_REGION_STATUS,"1 stock 2 2 0 saved\n");ha_load(&a);ha_load(&b);
 ha_region_set(&a,1);ha_region_set(&b,2);
 assert(a.region_wait&&b.region_wait&&a.region_seq==b.region_seq);
 assert(strcmp(a.action_session,b.action_session));
 ha_region_poll(&a);assert(a.region_wait&&!strcmp(a.region_choice,"A")); /* stale global ACK ignored */
 assert(ub_action_receive(UB_REGION_REQUEST,buf,sizeof buf,reply,sizeof reply));
 assert(strstr(reply,".r.")&&strstr(buf,"set "));
 assert(ub_action_receive(UB_REGION_REQUEST,buf,sizeof buf,path,sizeof path));
 assert(strcmp(path,reply)); /* neither client's request overwrote the other */
 reply_to(&b,UB_REGION_REQUEST,b.region_seq,"1 B 2 2 0 saved\n");
 ha_region_poll(&a);assert(a.region_wait);ha_region_poll(&b);assert(!b.region_wait&&!strcmp(b.region_choice,"B"));
 reply_to(&a,UB_REGION_REQUEST,a.region_seq,"1 A 2 2 0 not saved to USB\n");
 ha_region_poll(&a);assert(!a.region_wait&&strstr(a.notice,"not saved to USB"));
 ha_load(&dead);ha_region_set(&dead,1);assert(dead.region_wait);
 seconds+=29;ha_region_poll(&dead);assert(dead.region_wait);
 seconds++;assert(ha_region_poll(&dead)&&!dead.region_wait&&strstr(dead.notice,"result unknown"));
 assert(!ub_action_path(path,sizeof path,UB_REGION_REQUEST,'q',dead.action_session,dead.region_seq));assert(access(path,F_OK));
 /* DVD and Netflix use private replies and the same real deadline too. */
 put(UB_DVD_STATUS,"1 stock saved\n");put(UB_REMOTE_STATUS,"1 netflix saved\n");ha_load(&dead);
 ha_dvd_set(&dead,1);ha_netflix_set(&dead,2);ha_dvd_poll(&dead);ha_netflix_poll(&dead);assert(dead.dvd_wait&&dead.netflix_wait);
 seconds+=30;assert(ha_dvd_poll(&dead)&&ha_netflix_poll(&dead));assert(!dead.dvd_wait&&!dead.netflix_wait);
 /* SSH/log also ignore global replies from other menu sessions. */
 put(UB_SSH_STATUS,"0 0 stopped\n");ha_ssh_toggle(&dead);assert(dead.ssh_wait);ha_ssh_poll(&dead);assert(dead.ssh_wait);
 assert(ub_action_receive(UB_SSH_REQUEST,buf,sizeof buf,reply,sizeof reply));assert(!strcmp(buf,"start\n"));
 reply_to(&dead,UB_SSH_REQUEST,dead.ssh_seq,"1 0 running\n");assert(ha_ssh_poll(&dead)&&dead.ssh_running&&!dead.ssh_wait);
 put(UB_STORAGE_STATUS,"1 0 saved\n");ha_log_save(&dead);assert(dead.save_seq);ha_log_poll(&dead);assert(dead.save_seq);
 assert(ub_action_receive(UB_STORAGE_REQUEST,buf,sizeof buf,reply,sizeof reply));
 reply_to(&dead,UB_STORAGE_REQUEST,dead.save_seq,"1 -2 tool timed out\n");assert(ha_log_poll(&dead)&&!dead.save_seq&&strstr(dead.notice,"timed out"));
 ha_log_save(&dead);ha_ssh_toggle(&dead);seconds+=45;assert(ha_log_poll(&dead)&&ha_ssh_poll(&dead));assert(!dead.save_seq&&!dead.ssh_wait);
 /* Successful saves can use a per-run file when the previous log is kept. */
 ha_log_save(&dead);assert(ub_action_receive(UB_STORAGE_REQUEST,buf,sizeof buf,reply,sizeof reply));
 snprintf(buf,sizeof buf,"%u 0 saved to sub/ub820_submenu.fixture.log\n",dead.save_seq);
 reply_to(&dead,UB_STORAGE_REQUEST,dead.save_seq,buf);
 assert(ha_log_poll(&dead)&&!dead.save_seq&&strstr(dead.notice,"sub/ub820_submenu.fixture.log"));
 /* An absent client cannot leave executable stale requests or replies. */
 assert(!ub_action_path(path,sizeof path,UB_REGION_REQUEST,'q',a.action_session,99));assert(!ub_action_envelope(path,"set C 99\n",9,seconds+1));
 assert(!ub_action_path(reply,sizeof reply,UB_REGION_REQUEST,'r',a.action_session,98));assert(!ub_action_envelope(reply,"old",3,seconds+1));
 {char claimed[UB_ACTION_PATH];int len=snprintf(claimed,sizeof claimed,"%s.work",path);assert(len>0&&(size_t)len<sizeof claimed);assert(!ub_action_envelope(claimed,"old",3,seconds+1));seconds+=2;
  {char temporary[UB_ACTION_PATH];len=snprintf(temporary,sizeof temporary,"%s.tmp-1-%u",path,seconds-1);assert(len>0&&(size_t)len<sizeof temporary);put(temporary,"");
   assert(!ub_action_receive(UB_REGION_REQUEST,buf,sizeof buf,reply,sizeof reply));assert(access(path,F_OK)&&access(claimed,F_OK)&&access(temporary,F_OK));}}
 assert(!ub_action_path(reply,sizeof reply,UB_REGION_REQUEST,'r',a.action_session,98));assert(access(reply,F_OK));
 /* A new publication after claim survives cleanup of the old inode. */
 put(UB_REMOTE_REQUEST,"set nothing 1\n");replace_legacy=1;
 assert(ub_action_receive(UB_REMOTE_REQUEST,buf,sizeof buf,reply,sizeof reply));assert(!strcmp(buf,"set nothing 1\n")&&!reply[0]);
 assert(ub_action_receive(UB_REMOTE_REQUEST,buf,sizeof buf,reply,sizeof reply));assert(!strcmp(buf,"set helper 2\n"));
 assert(!ub_action_suffix("a"));assert(!ub_action_suffix("00000000000000000000000000000000.1.work"));
 unlink(UB_REGION_STATUS);unlink(UB_DVD_STATUS);unlink(UB_REMOTE_STATUS);unlink(UB_SSH_STATUS);unlink(UB_STORAGE_STATUS);rmdir(UB_ACTION_TEST_DIR);
 puts("action_ipc_test passed");return 0;
}
