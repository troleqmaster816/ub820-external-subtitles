/* Actual CPU Helper overlay and action protocol, with own RAM-file fixtures.
 * Existing CTK stubs are reused; the power regression main is not executed. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "CPU UI fixture requires UB_HOST_TEST"
#endif
#include "helper_menu.h"
#undef UB_CPU_ENABLED
#undef UB_CPU_REQUEST
#undef UB_CPU_STATUS
static char cpu_enabled_path[256],cpu_request_path[256],cpu_status_path[256];
#define UB_CPU_ENABLED cpu_enabled_path
#define UB_CPU_REQUEST cpu_request_path
#define UB_CPU_STATUS cpu_status_path
#define main power_ui_regression_main
#include "power_ui_test.c"
#undef main

static void cpu_reply(struct helper_state *state,unsigned sequence,const char *body){
 char path[UB_ACTION_PATH];assert(!ub_action_path(path,sizeof path,UB_CPU_REQUEST,'r',state->action_session,sequence));assert(!ub_action_reply(path,body,strlen(body)));
}
int main(void){
 char folder[]="/tmp/ub820-cpu-ui-XXXXXX",text[192],reply[UB_ACTION_PATH],label[128];
 unsigned ids[HELPER_ROWS],seq,choice,cases=0;int known;const char *message;
 struct helper_state a={0},b={0},fresh={0},same={0};
 assert(mkdtemp(folder));snprintf(cpu_enabled_path,sizeof cpu_enabled_path,"%s/enabled",folder);snprintf(cpu_request_path,sizeof cpu_request_path,"%s/request",folder);snprintf(cpu_status_path,sizeof cpu_status_path,"%s/status",folder);
 assert(H_REGION==0&&H_DVD==1&&H_NETFLIX==2&&H_SSH==3&&H_LOG==4&&H_SAVE==5&&H_POWER==6&&H_CPU==7);
 hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;
 assert(helper_rows(ids)==7&&ids[3]==H_POWER&&ids[4]==H_SSH&&ids[6]==H_SAVE);cases++;
 assert(!ha_cpu_poll(&a)&&!a.cpu_enabled&&!a.cpu_known&&!strcmp(ha_cpu_value(&a),"unavailable"));ha_cpu_set(&a,1);assert(!a.cpu_wait);cases++;
 ha_load(&fresh);assert(!fresh.cpu_enabled&&!fresh.cpu_wait&&!fresh.cpu_counter);cases++;
 put(cpu_enabled_path,"");assert(ha_cpu_poll(&a)&&a.cpu_enabled&&!a.cpu_known);
 hs.cpu_enabled=1;assert(helper_rows(ids)==8&&ids[4]==H_CPU&&ids[5]==H_SSH&&ids[7]==H_SAVE&&HELPER_ROWS<=VIEW_ROWS);cases++;
 put(cpu_status_path,"0 unknown module not ready\n");ha_cpu_poll(&a);assert(!a.cpu_known&&!a.cpu_wait);
 hs=a;open_drop(DROP_CPU);assert(m.drop_kind==DROP_NONE&&strstr(hs.notice,"module not ready"));cases++;
 put(cpu_status_path,"0 off no instance\n");assert(ha_cpu_poll(&a)&&a.cpu_known&&!a.cpu_choice);
 ha_cpu_set(&a,2);assert(!a.cpu_wait&&!a.cpu_counter);cases++;
 /* Live Off can coexist with a suppressed saved On preference. An explicit
  * Off must still be delivered and acknowledged to change that preference. */
 assert(ha_cpu_poll(&same)&&same.cpu_known&&!same.cpu_choice);
 ha_cpu_set(&same,0);assert(same.cpu_wait&&!same.cpu_choice&&same.cpu_known);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"set off 1\n"));cases++;
 cpu_reply(&same,same.cpu_seq,"1 off already applied; saved\n");
 assert(ha_cpu_poll(&same)&&!same.cpu_wait&&same.cpu_known&&!same.cpu_choice&&strstr(same.notice,"saved"));cases++;
 ha_load(&fresh);assert(fresh.cpu_query&&fresh.cpu_wait&&!fresh.cpu_known&&!strcmp(ha_cpu_value(&fresh),"checking..."));
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"get 1\n"));cases++;
 put(cpu_status_path,"999 on stale\n");ha_cpu_poll(&fresh);assert(fresh.cpu_wait&&!fresh.cpu_known);cases++;
 ha_copy(fresh.notice,"Saving another setting...",sizeof fresh.notice);cpu_reply(&fresh,fresh.cpu_seq,"1 off verified\n");
 assert(ha_cpu_poll(&fresh)&&fresh.cpu_known&&!fresh.cpu_choice&&!fresh.cpu_wait&&!strcmp(fresh.notice,"Saving another setting..."));cases++;
 for(unsigned i=0;i<20;i++)ha_cpu_poll(&fresh);
 assert(fresh.cpu_confirmed&&fresh.cpu_known&&!fresh.cpu_choice&&!fresh.cpu_wait&&fresh.cpu_counter==1);cases++;
 put(cpu_status_path,"1000 unknown native state changed\n");assert(ha_cpu_poll(&fresh)&&!fresh.cpu_known&&!fresh.cpu_confirmed&&fresh.cpu_uncertain);put(cpu_status_path,"0 off current\n");cases++;
 ha_cpu_set(&a,1);assert(a.cpu_wait&&!a.cpu_choice&&a.cpu_known);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"set on 1\n"));cases++;
 put(cpu_status_path,"1 on saved\n");ha_cpu_poll(&a);assert(a.cpu_wait&&!a.cpu_choice);cases++;
 cpu_reply(&a,a.cpu_seq,"2 on saved\n");ha_cpu_poll(&a);assert(a.cpu_wait&&!a.cpu_choice);cases++;
 ha_cpu_poll(&b);assert(b.cpu_known&&b.cpu_choice);ha_cpu_set(&b,0);assert(b.cpu_wait);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"set off 1\n"));
 cpu_reply(&b,b.cpu_seq,"1 off saved\n");ha_cpu_poll(&a);assert(a.cpu_wait);assert(ha_cpu_poll(&b)&&!b.cpu_wait&&!b.cpu_choice);cases++;
 cpu_reply(&a,a.cpu_seq,"1 on saved\n");assert(ha_cpu_poll(&a)&&!a.cpu_wait&&a.cpu_choice&&a.cpu_known);cases++;
 hs=a;assert(!strcmp(helper_text(H_CPU,label,sizeof label),"CPU optimization: On"));open_drop(DROP_CPU);assert(m.drop_count==2&&m.drop_sel==0&&!strcmp(drop_text(0),"On")&&!strcmp(drop_text(1),"Off"));m.drop_kind=DROP_NONE;cases++;
 ha_cpu_set(&a,0);assert(a.cpu_wait&&a.cpu_choice);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"set off 2\n"));
 cpu_reply(&a,a.cpu_seq,"2 unknown native state unhealthy; restart player required\n");assert(ha_cpu_poll(&a)&&!a.cpu_wait&&!a.cpu_known&&a.cpu_choice&&a.cpu_uncertain&&strstr(a.notice,"restart player required"));cases++;
 hs=a;open_drop(DROP_CPU);assert(m.drop_kind==DROP_NONE&&!strcmp(ha_cpu_value(&hs),"unavailable")&&strstr(hs.notice,"restart player required"));cases++;
 put(cpu_status_path,"2 off verified\n");ha_cpu_poll(&a);assert(!a.cpu_known&&a.cpu_uncertain);
 ha_load(&a);assert(a.cpu_query&&a.cpu_wait&&!a.cpu_known&&!a.cpu_uncertain);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"get 3\n"));
 cpu_reply(&a,a.cpu_seq,"3 off verified\n");assert(ha_cpu_poll(&a)&&a.cpu_known&&!a.cpu_choice&&!a.cpu_wait&&!a.cpu_uncertain);cases++;
 ha_cpu_set(&a,1);assert(a.cpu_wait&&!a.cpu_choice);seconds+=30;
 assert(ha_cpu_poll(&a)&&!a.cpu_wait&&!a.cpu_known&&a.cpu_uncertain&&strstr(a.notice,"reopen Helper"));ha_cpu_set(&a,1);assert(!a.cpu_wait);cases++;
 snprintf(text,sizeof text,"%u on saved\n",a.cpu_seq);cpu_reply(&a,a.cpu_seq,text);put(cpu_status_path,text);ha_cpu_poll(&a);assert(!a.cpu_known);ha_load(&a);assert(a.cpu_query&&!a.cpu_known);
 assert(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)&&!strcmp(text,"get 5\n"));cpu_reply(&a,a.cpu_seq,"5 on verified\n");assert(ha_cpu_poll(&a)&&a.cpu_known&&a.cpu_choice&&!a.cpu_wait);cases++;
 ha_cpu_set(&a,0);assert(a.cpu_wait);assert(!unlink(cpu_enabled_path));assert(ha_cpu_poll(&a)&&!a.cpu_known&&!a.cpu_wait&&!a.cpu_enabled&&a.cpu_uncertain);cases++;
 hs=a;hs.dvd_known=hs.netflix_known=hs.power_enabled=hs.ssh_enabled=1;
 m.kind=PAGE_HELPER;m.active=1;m.window=(Widget)(uintptr_t)1;m.list=(Widget)(uintptr_t)2;m.list_fixed=(Widget)(uintptr_t)3;m.drop_kind=DROP_CPU;m.selected=7;
 render();assert(m.drop_kind==DROP_NONE&&!m.list&&m.selected==6);cases++;
 hs.cpu_enabled=1;hs.cpu_known=0;m.list=(Widget)(uintptr_t)2;m.list_fixed=(Widget)(uintptr_t)3;m.drop_kind=DROP_CPU;
 render();assert(m.drop_kind==DROP_NONE&&!m.list&&helper_rows(ids)==8&&!strcmp(ha_cpu_value(&hs),"unavailable"));m.active=0;m.window=0;cases++;
 assert(!ha_cpu_status("4294967295 unknown state missing",&seq,&choice,&known,&message)&&seq==UINT32_MAX&&!known);
 assert(ha_cpu_status("4294967296 off saved",&seq,&choice,&known,&message));assert(ha_cpu_status("-1 on saved",&seq,&choice,&known,&message));assert(ha_cpu_status("1 unknownish error",&seq,&choice,&known,&message));assert(ha_cpu_status("1 on",&seq,&choice,&known,&message));cases++;
 ha_action_cancel(&a,UB_CPU_REQUEST,a.cpu_seq);ha_action_cancel(&b,UB_CPU_REQUEST,b.cpu_seq);seconds+=100;while(ub_action_receive(UB_CPU_REQUEST,text,sizeof text,reply,sizeof reply)){}
 unlink(cpu_status_path);assert(!rmdir(folder));printf("cpu_ui_test passed (%u scenarios)\n",cases);return 0;
}
