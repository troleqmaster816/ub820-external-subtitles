/* Real settings submenu callbacks with stock GTK fixture functions. Only own
 * temporary power IPC files are used; no player, media or firmware execution. */
#define _GNU_SOURCE
#ifndef UB_HOST_TEST
#error "Settings power fixture requires UB_HOST_TEST"
#endif
#include <stdlib.h>
#include <unistd.h>
#include "helper_menu.h"
#undef UB_POWER_ENABLED
#undef UB_POWER_REQUEST
#undef UB_POWER_STATUS
static char power_enabled_path[256],power_request_path[256],power_status_path[256];
#define UB_POWER_ENABLED power_enabled_path
#define UB_POWER_REQUEST power_request_path
#define UB_POWER_STATUS power_status_path
#include "action_ipc.h"
/* Reuse signature-checked GTK stubs and native-tile setup. The regression main
 * is deliberately not invoked here: these fixtures exercise only power rows. */
#define main settings_tile_regression_main
#include "settings_tile_test.c"
#undef main

static void put_power(const char *path,const char *text){
 FILE *file=fopen(path,"w");assert(file);assert(fputs(text,file)>=0);assert(!fclose(file));
}
static void power_reply(unsigned sequence,const char *body){
 char path[UB_ACTION_PATH];assert(!ub_action_path(path,sizeof path,UB_POWER_REQUEST,'r',hs.action_session,sequence));
 assert(!ub_action_reply(path,body,strlen(body)));
}
int main(void){
 char folder[]="/tmp/ub820-power-settings-XXXXXX",body[80],reply[UB_ACTION_PATH];
 unsigned int ids[SUB_ROWS_MAX],checks=0;
 assert(mkdtemp(folder));
 snprintf(power_enabled_path,sizeof power_enabled_path,"%s/enabled",folder);
 snprintf(power_request_path,sizeof power_request_path,"%s/request",folder);
 snprintf(power_status_path,sizeof power_status_path,"%s/status",folder);
 reset();memset(&hs,0,sizeof hs);
 assert(S_REGION==0&&S_DVD==1&&S_NETFLIX==2&&S_SSH==3&&S_LOG==4&&S_SAVE==5&&S_POWER==6);
 assert(LIST_NONE==0&&LIST_REGION==1&&LIST_NETFLIX==2&&LIST_DVD==3&&LIST_POWER==4);checks++;
 hs.dvd_known=hs.netflix_known=hs.ssh_enabled=1;
 assert(sub_rows(ids)==6&&ids[2]==S_NETFLIX&&ids[3]==S_SSH&&ids[5]==S_SAVE);checks++;
 hs.power_enabled=1;
 assert(sub_rows(ids)==7&&ids[3]==S_POWER&&ids[4]==S_SSH&&ids[6]==S_SAVE);
 assert(SUB_ROWS_MAX<=ROWS&&46+(SUB_ROWS_MAX-1)*34+30<SUB_H-24);checks++;
 s.selected=3;s.open=0;s.listing=LIST_NONE;
 assert(sub_ok()==1&&s.listing==LIST_NONE&&strstr(hs.notice,"not ready"));checks++;
 hs.power_known=1;hs.power_choice=1;hs.notice[0]=0;
 assert(sub_ok()==1&&s.listing==LIST_POWER&&s.drop_sel==0);
 assert(list_count()==2&&!strcmp(list_label(0),"On")&&!strcmp(list_label(1),"Off"));checks++;
 memset(hs.action_session,'a',UB_ACTION_SESSION);hs.action_session[UB_ACTION_SESSION]=0;
 put_power(power_enabled_path,"");put_power(power_status_path,"0 on ready\n");
 s.drop_sel=1;assert(sub_ok()==1&&s.listing==LIST_NONE&&hs.power_wait&&hs.power_choice==1);checks++;
 assert(ub_action_receive(UB_POWER_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"set off 1\n"));checks++;
 put_power(power_status_path,"1 off saved\n");ha_power_poll(&hs);
 assert(hs.power_wait&&hs.power_choice==1);checks++;
 power_reply(hs.power_seq,"2 off saved\n");ha_power_poll(&hs);
 assert(hs.power_wait&&hs.power_choice==1);checks++;
 power_reply(hs.power_seq,"1 off saved\n");assert(ha_power_poll(&hs)&&!hs.power_wait&&hs.power_choice==0);checks++;
 assert(sub_ok()==1&&s.listing==LIST_POWER&&s.drop_sel==1);checks++;
 s.drop_sel=0;assert(sub_ok()==1&&s.listing==LIST_NONE&&hs.power_wait&&hs.power_choice==0);
 assert(ub_action_receive(UB_POWER_REQUEST,body,sizeof body,reply,sizeof reply)&&!strcmp(body,"set on 2\n"));checks++;
 assert(!unlink(power_enabled_path));assert(ha_power_poll(&hs)&&!hs.power_wait&&!hs.power_known&&hs.power_uncertain);checks++;
 hs.power_enabled=1;s.listing=LIST_NONE;
 assert(sub_ok()==1&&s.listing==LIST_NONE&&strstr(hs.notice,"not ready"));checks++;
 /* A backend disappearing while its dropdown is open must not leave the
  * old popup attached to a different row after the row list contracts. */
 hs.power_enabled=0;hs.power_known=0;s.open=1;s.listing=LIST_POWER;
 s.list=(Widget)1;s.list_fixed=(Widget)2;s.selected=6;
 render_sub();
 assert(s.listing==LIST_NONE&&!s.list&&!s.list_fixed&&sub_rows(ids)==6&&s.selected==5&&!hs.power_wait);checks++;
 ha_action_cancel(&hs,UB_POWER_REQUEST,hs.power_seq);unlink(power_status_path);
 assert(!rmdir(folder));
 printf("power_settings_test passed (%u scenarios)\n",checks);return 0;
}
