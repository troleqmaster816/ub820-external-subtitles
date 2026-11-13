/* Actual Settings CPU row and callbacks; only own temp action files. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "CPU Settings fixture requires UB_HOST_TEST"
#endif
#include <assert.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include "helper_menu.h"
#undef UB_CPU_ENABLED
#undef UB_CPU_REQUEST
#undef UB_CPU_STATUS
static char cpu_enabled_path[256],cpu_request_path[256],cpu_status_path[256];
#define UB_CPU_ENABLED cpu_enabled_path
#define UB_CPU_REQUEST cpu_request_path
#define UB_CPU_STATUS cpu_status_path
#undef UB_POWER_ENABLED
#undef UB_POWER_REQUEST
#undef UB_POWER_STATUS
static char power_enabled_path[256],power_request_path[256],power_status_path[256];
#define UB_POWER_ENABLED power_enabled_path
#define UB_POWER_REQUEST power_request_path
#define UB_POWER_STATUS power_status_path
static uint32_t cpu_seconds=100;
static void cpu_server_now(struct timespec *out){out->tv_sec=cpu_seconds;out->tv_nsec=0;}
static int cpu_clock(clockid_t id,struct timespec *out){assert(id==CLOCK_MONOTONIC);cpu_server_now(out);return 0;}
#define ub_now cpu_server_now
#define clock_gettime cpu_clock
#include "action_ipc.h"
#define main settings_tile_regression_main
#include "settings_tile_test.c"
#undef main
#undef clock_gettime
#undef ub_now

static void put_power(const char *path,const char *text){FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void cpu_reply(unsigned sequence,const char *body){char path[UB_ACTION_PATH];assert(!ub_action_path(path,sizeof path,UB_CPU_REQUEST,'r',hs.action_session,sequence));assert(!ub_action_reply(path,body,strlen(body)));}
int main(void){
 char folder[]="/tmp/ub820-cpu-settings-XXXXXX",body[192],reply[UB_ACTION_PATH];unsigned ids[SUB_ROWS_MAX],cases=0;
 assert(mkdtemp(folder));snprintf(cpu_enabled_path,sizeof cpu_enabled_path,"%s/enabled",folder);snprintf(cpu_request_path,sizeof cpu_request_path,"%s/request",folder);snprintf(cpu_status_path,sizeof cpu_status_path,"%s/status",folder);
 reset();memset(&hs,0,sizeof hs);
 ha_load(&hs);assert(!hs.cpu_enabled&&!hs.cpu_wait&&!hs.cpu_counter);cases++;
 assert(S_POWER==6&&S_CPU==7&&LIST_POWER==4&&LIST_CPU==5);hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;
 assert(sub_rows(ids)==7&&ids[3]==S_POWER&&ids[4]==S_SSH&&ids[6]==S_SAVE);cases++;
 hs.cpu_enabled=1;assert(sub_rows(ids)==8&&ids[4]==S_CPU&&ids[5]==S_SSH&&ids[7]==S_SAVE);
 assert(SUB_ROWS_MAX<=ROWS&&46+(SUB_ROWS_MAX-1)*34+30<SUB_H-24&&46+ROWS*34+4+56<=SUB_H);cases++;
 s.selected=4;s.open=0;ha_copy(hs.cpu_message,"backend not ready",sizeof hs.cpu_message);
 assert(sub_ok()==1&&s.listing==LIST_NONE&&strstr(hs.notice,"backend not ready"));cases++;
 put_power(cpu_enabled_path,"");put_power(cpu_status_path,"0 off no instance\n");assert(ha_cpu_poll(&hs)&&hs.cpu_known&&!hs.cpu_choice);
 ha_load(&hs);assert(hs.cpu_query&&hs.cpu_wait&&!hs.cpu_known&&!strcmp(ha_cpu_value(&hs),"checking..."));
 assert(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"get 1\n"));
 put_power(cpu_status_path,"1 on stale\n");ha_cpu_poll(&hs);assert(hs.cpu_wait&&!hs.cpu_known);
 cpu_reply(hs.cpu_seq,"1 off verified\n");assert(ha_cpu_poll(&hs)&&hs.cpu_known&&!hs.cpu_choice&&!hs.cpu_wait);cases++;
 ha_cpu_poll(&hs);assert(hs.cpu_confirmed&&hs.cpu_known&&!hs.cpu_choice&&!hs.cpu_wait&&hs.cpu_counter==1);cases++;
 hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;
 assert(sub_ok()==1&&s.listing==LIST_CPU&&s.drop_sel==1&&list_count()==2&&!strcmp(list_label(0),"On")&&!strcmp(list_label(1),"Off"));cases++;
 s.drop_sel=0;assert(sub_ok()==1&&s.listing==LIST_NONE&&hs.cpu_wait&&!hs.cpu_choice);
 assert(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"set on 2\n"));cases++;
 put_power(cpu_status_path,"2 on saved\n");ha_cpu_poll(&hs);assert(hs.cpu_wait&&!hs.cpu_choice);cases++;
 cpu_reply(hs.cpu_seq,"3 on saved\n");ha_cpu_poll(&hs);assert(hs.cpu_wait&&!hs.cpu_choice);cases++;
 cpu_reply(hs.cpu_seq,"2 on saved\n");assert(ha_cpu_poll(&hs)&&!hs.cpu_wait&&hs.cpu_choice);cases++;
 assert(sub_ok()==1&&s.listing==LIST_CPU&&s.drop_sel==0);s.drop_sel=1;assert(sub_ok()==1&&hs.cpu_wait&&hs.cpu_choice);
 assert(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"set off 3\n"));
 cpu_reply(hs.cpu_seq,"3 unknown native state unhealthy; restart player required\n");assert(ha_cpu_poll(&hs)&&!hs.cpu_known&&hs.cpu_choice&&!hs.cpu_wait&&strstr(hs.notice,"restart player required"));cases++;
 assert(sub_ok()==1&&s.listing==LIST_NONE&&!strcmp(ha_cpu_value(&hs),"unavailable")&&strstr(hs.notice,"restart player required"));cases++;
 s.open=1;s.listing=LIST_CPU;s.list=(Widget)1;s.list_fixed=(Widget)2;render_sub();assert(s.listing==LIST_NONE&&!s.list&&!s.list_fixed&&sub_rows(ids)==8);cases++;
 hs.cpu_uncertain=0;put_power(cpu_status_path,"3 off current\n");ha_cpu_poll(&hs);assert(hs.cpu_known&&!hs.cpu_choice);s.open=0;assert(sub_ok()==1&&s.listing==LIST_CPU);s.drop_sel=0;sub_ok();assert(hs.cpu_wait);
 cpu_seconds+=30;assert(ha_cpu_poll(&hs)&&!hs.cpu_wait&&!hs.cpu_known&&hs.cpu_uncertain);cases++;
 ha_load(&hs);assert(hs.cpu_query&&!hs.cpu_known&&hs.cpu_wait);
 assert(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"get 5\n"));cpu_reply(hs.cpu_seq,"5 off verified\n");assert(ha_cpu_poll(&hs)&&hs.cpu_known&&!hs.cpu_choice&&!hs.cpu_wait);
 /* Confirming the already-current Off still saves an explicit preference. */
 hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;
 s.selected=4;s.open=0;s.listing=LIST_NONE;
 assert(sub_ok()==1&&s.listing==LIST_CPU&&s.drop_sel==1);
 assert(sub_ok()==1&&s.listing==LIST_NONE&&hs.cpu_wait&&!hs.cpu_choice);
 assert(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"set off 6\n"));
 cpu_reply(hs.cpu_seq,"6 off already applied; saved\n");assert(ha_cpu_poll(&hs)&&hs.cpu_known&&!hs.cpu_choice&&!hs.cpu_wait&&strstr(hs.notice,"saved"));cases++;
 hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;s.open=1;s.selected=7;s.listing=LIST_NONE;
 assert(sub_down()==1&&s.selected==0);assert(sub_up()==1&&s.selected==7);cases++;
 assert(!unlink(cpu_enabled_path));ha_cpu_poll(&hs);s.listing=LIST_CPU;s.list=(Widget)1;s.list_fixed=(Widget)2;
 render_sub();assert(s.listing==LIST_NONE&&!s.list&&sub_rows(ids)==7&&s.selected==6);cases++;
 ha_action_cancel(&hs,UB_CPU_REQUEST,hs.cpu_seq);cpu_seconds+=100;while(ub_action_receive(UB_CPU_REQUEST,body,sizeof body,reply,sizeof reply)){}
 unlink(cpu_status_path);assert(!rmdir(folder));printf("cpu_settings_test passed (%u scenarios)\n",cases);return 0;
}
