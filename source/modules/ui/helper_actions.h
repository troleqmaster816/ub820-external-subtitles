/* UB820 Helper actions for a menu payload: Blu-ray and DVD region, Netflix button,
 * SSH and the helper log, through the request and status files the helper's modules use
 * (helper_menu.h, ssh.h, storage.h). Included by a payload after its libc
 * declarations; everything is static. No integer division (the player
 * programs do not export __aeabi_uidivmod). */
#ifndef UB_HELPER_ACTIONS_H
#define UB_HELPER_ACTIONS_H
#include "helper_menu.h"
#include "ssh.h"
#define UB_PAYLOAD
#include "storage.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "payload_time.h"
#include "action_ipc.h"

#define HA_SSH_ENABLED "/tmp/ub820_ssh.enabled"
struct helper_state {
 char action_session[UB_ACTION_SESSION+1];
 uint32_t region_deadline,dvd_deadline,netflix_deadline,ssh_deadline,save_deadline,power_deadline,cpu_deadline;
 unsigned int ssh_seq,ssh_counter;
 int ssh_enabled,ssh_running; unsigned int ssh_wait;
 int log_always; unsigned int save_seq,save_ticks,save_counter;
 int region_known,region_auto; char region_choice[8]; unsigned int region_disc,region_player,region_seq,region_wait,region_counter;
 int netflix_known; unsigned int netflix_choice,netflix_seq,netflix_wait,netflix_counter;
 int dvd_known; unsigned int dvd_choice,dvd_seq,dvd_wait,dvd_counter;
 int power_enabled,power_known,power_uncertain; unsigned int power_choice,power_seq,power_wait,power_counter;
 int cpu_enabled,cpu_known,cpu_uncertain,cpu_query,cpu_confirmed; unsigned int cpu_choice,cpu_seq,cpu_wait,cpu_counter;
 char cpu_message[96];
 char notice[128];
};

static void ha_copy(char *out,const char *in,size_t n){size_t i=0;if(!n)return;while(i+1<n&&in&&in[i]){out[i]=in[i];i++;}out[i]=0;}
static int ha_exists(const char *path){int fd=open(path,O_RDONLY);if(fd>=0)close(fd);return fd>=0;}
/* First line of a small text file; 0 if absent. */
static int ha_read_line(const char *path,char *buf,size_t cap){
 int fd=open(path,O_RDONLY);ssize_t n;
 if(fd<0)return 0;
 n=read(fd,buf,cap-1);close(fd);if(n<=0)return 0;
 buf[n]=0;buf[strcspn(buf,"\n")]=0;return 1;
}
/* Random session IDs prevent acknowledgements from a previous payload or
 * another menu from matching even when both local counters start at one. */
static int ha_action_session(struct helper_state *s){
 unsigned char random[16];static const char hex[]="0123456789abcdef";size_t done=0;ssize_t n;int fd;
 if(s->action_session[0])return 0;
 fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
 while(done<sizeof random){n=read(fd,random+done,sizeof random-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return -1;}done+=(size_t)n;}
 close(fd);for(done=0;done<sizeof random;done++){s->action_session[2*done]=hex[random[done]>>4];s->action_session[2*done+1]=hex[random[done]&15];}
 s->action_session[UB_ACTION_SESSION]=0;return 0;
}
static int ha_action_now(uint32_t *now){return ub_payload_now(now);}
static int ha_action_send_timeout(struct helper_state *s,const char *base,const char *text,size_t n,unsigned seq,uint32_t *deadline,uint32_t timeout){
 char path[UB_ACTION_PATH];uint32_t now;
 if(ha_action_now(&now)||ha_action_session(s)||ub_action_path(path,sizeof path,base,'q',s->action_session,seq))return -1;
 *deadline=now+timeout;
 return ub_action_envelope(path,text,n,*deadline);
}
static int ha_action_send(struct helper_state *s,const char *base,const char *text,size_t n,unsigned seq,uint32_t *deadline){return ha_action_send_timeout(s,base,text,n,seq,deadline,30u);}
static void ha_action_cancel(const struct helper_state *s,const char *base,unsigned seq){
 char path[UB_ACTION_PATH];if(!s->action_session[0]||!seq)return;
 if(!ub_action_path(path,sizeof path,base,'q',s->action_session,seq))unlink(path);
 if(!ub_action_path(path,sizeof path,base,'r',s->action_session,seq))unlink(path);
}
/* 1 our reply, 0 pending, -1 expired/unreadable clock. The queue file can
 * already have been claimed at timeout: do not claim the change was undone. */
static int ha_action_poll(const struct helper_state *s,const char *base,unsigned seq,uint32_t deadline,char *buf,size_t cap){
 char path[UB_ACTION_PATH],message[512],*body;uint32_t now,expires;int fd;ssize_t n;
 if(ub_action_path(path,sizeof path,base,'r',s->action_session,seq)||ha_action_now(&now))return -1;
 fd=open(path,O_RDONLY);
 if(fd>=0){
  do{n=read(fd,message,sizeof message-1);}while(n<0&&errno==EINTR);
  close(fd);
  if(n>0){
   message[n]=0;body=ub_action_body(message,&expires);
   if(body&&(int32_t)(now-expires)<0){ha_copy(buf,body,cap);buf[strcspn(buf,"\n")]=0;unlink(path);return 1;}
  }
 }
 if((int32_t)(now-deadline)>=0){ha_action_cancel(s,base,seq);return -1;}
 return 0;
}
static unsigned int ha_parse(const char **p){unsigned int v=0;while(**p>='0'&&**p<='9')v=v*10+(unsigned int)(*(*p)++-'0');return v;}

/* ---- Idle auto-power-off ---- */
static const char *ha_power_value(const struct helper_state *s){
 return !s->power_enabled?"not available":!s->power_known?"not confirmed":s->power_choice?"On":"Off";
}
/* Strictly parse the new protocol; an invalid or different private reply
 * never acknowledges this menu's pending request. */
static int ha_power_status(const char *text,unsigned int *seq,unsigned int *choice,const char **message){
 const char *p=text;unsigned int value=0;
 if(*p<'0'||*p>'9')return -1;
 do{unsigned int digit=(unsigned int)(*p-'0');if(value>429496729u||(value==429496729u&&digit>5u))return -1;value=value*10u+digit;p++;}while(*p>='0'&&*p<='9');
 if(*p++!=' ')return -1;
 if(!strncmp(p,"on ",3)){*choice=1;p+=3;}
 else if(!strncmp(p,"off ",4)){*choice=0;p+=4;}
 else return -1;
 *seq=value;*message=p;return 0;
}
static int ha_power_poll(struct helper_state *s){
 char text[192];const char *message;unsigned int seq,choice;int enabled=ha_exists(UB_POWER_ENABLED),changed=enabled!=s->power_enabled,reply=0;
 s->power_enabled=enabled;
 if(!enabled){
  changed|=s->power_known;s->power_known=0;
  if(s->power_wait){ha_action_cancel(s,UB_POWER_REQUEST,s->power_seq);s->power_wait=0;s->power_uncertain=1;ha_copy(s->notice,"Power control unavailable; result unknown",sizeof s->notice);return 1;}
  return changed;
 }
 if(s->power_wait){
  reply=ha_action_poll(s,UB_POWER_REQUEST,s->power_seq,s->power_deadline,text,sizeof text);
  if(reply<0){s->power_wait=0;s->power_known=0;s->power_uncertain=1;ha_copy(s->notice,"No power confirmation; reopen Helper to refresh",sizeof s->notice);return 1;}
  if(!reply||ha_power_status(text,&seq,&choice,&message)||seq!=s->power_seq)return changed;
  s->power_choice=choice;s->power_known=1;s->power_wait=0;s->power_uncertain=0;
  if(!strcmp(message,"saved"))snprintf(s->notice,sizeof s->notice,"Auto power-off: %s. Manual POWER is unchanged.",ha_power_value(s));
  else snprintf(s->notice,sizeof s->notice,"Auto power-off: %s, %.90s",ha_power_value(s),message);
  return 1;
 }
 /* A stale global observation must not turn a timed-out action into success.
  * Reopening the page explicitly refreshes the observational state. */
 if(s->power_uncertain)return changed;
 if(!ha_read_line(UB_POWER_STATUS,text,sizeof text)||ha_power_status(text,&seq,&choice,&message)){
  changed|=s->power_known;s->power_known=0;return changed;
 }
 changed|=!s->power_known||choice!=s->power_choice;s->power_choice=choice;s->power_known=1;return changed;
}
static void ha_power_set(struct helper_state *s,unsigned int next){
 char text[32];int n;
 if(!s->power_enabled||!s->power_known){ha_copy(s->notice,"Power control is not ready; reopen Helper to refresh",sizeof s->notice);return;}
 if(s->power_wait){ha_copy(s->notice,"Saving...",sizeof s->notice);return;}
 if(next>1||next==s->power_choice)return;
 if(!++s->power_counter)s->power_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",next?"on":"off",s->power_counter);
 if(ha_action_send(s,UB_POWER_REQUEST,text,(size_t)n,s->power_counter,&s->power_deadline)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->power_seq=s->power_counter;s->power_wait=1;ha_copy(s->notice,"Saving...",sizeof s->notice);
}

/* ---- CPU optimization ---- */
static const char *ha_cpu_value(const struct helper_state *s){
 if(s->cpu_enabled&&s->cpu_wait&&s->cpu_query)return "checking...";
 return !s->cpu_enabled||!s->cpu_known?"unavailable":s->cpu_choice?"On":"Off";
}
static void ha_cpu_unavailable(struct helper_state *s){
 if(s->cpu_wait&&s->cpu_query){ha_copy(s->notice,"Checking CPU optimization...",sizeof s->notice);return;}
 snprintf(s->notice,sizeof s->notice,"CPU optimization unavailable: %.90s",s->cpu_message[0]?s->cpu_message:"reopen Helper to refresh");
}
static int ha_cpu_status(const char *text,unsigned int *seq,unsigned int *choice,int *known,const char **message){
 const char *p=text;unsigned int value=0;
 if(*p<'0'||*p>'9')return -1;
 do{unsigned int digit=(unsigned int)(*p-'0');if(value>429496729u||(value==429496729u&&digit>5u))return -1;value=value*10u+digit;p++;}while(*p>='0'&&*p<='9');
 if(*p++!=' ')return -1;
 if(!strncmp(p,"on ",3)){*choice=1;*known=1;p+=3;}
 else if(!strncmp(p,"off ",4)){*choice=0;*known=1;p+=4;}
 else if(!strncmp(p,"unknown ",8)){*choice=0;*known=0;p+=8;}
 else return -1;
 *seq=value;*message=p;return 0;
}
static int ha_cpu_poll(struct helper_state *s){
 char text[192];const char *message;unsigned int seq,choice;int known,enabled=ha_exists(UB_CPU_ENABLED),changed=enabled!=s->cpu_enabled,reply,query;
 s->cpu_enabled=enabled;
 if(!enabled){
  changed|=s->cpu_known;s->cpu_known=0;s->cpu_confirmed=0;
  if(s->cpu_wait){ha_action_cancel(s,UB_CPU_REQUEST,s->cpu_seq);s->cpu_wait=0;s->cpu_query=0;s->cpu_uncertain=1;ha_copy(s->cpu_message,"module unavailable; result unknown",sizeof s->cpu_message);ha_cpu_unavailable(s);return 1;}
  return changed;
 }
 if(s->cpu_wait){
  reply=ha_action_poll(s,UB_CPU_REQUEST,s->cpu_seq,s->cpu_deadline,text,sizeof text);
  if(reply<0){s->cpu_wait=0;s->cpu_query=0;s->cpu_known=0;s->cpu_confirmed=0;s->cpu_uncertain=1;ha_copy(s->cpu_message,"no confirmation; reopen Helper to refresh",sizeof s->cpu_message);ha_cpu_unavailable(s);return 1;}
  if(!reply||ha_cpu_status(text,&seq,&choice,&known,&message)||seq!=s->cpu_seq)return changed;
  query=s->cpu_query;
  s->cpu_wait=0;s->cpu_query=0;s->cpu_known=known;s->cpu_confirmed=known;s->cpu_uncertain=!known;ha_copy(s->cpu_message,message,sizeof s->cpu_message);
  if(!known){if(!query||!strcmp(s->notice,"Checking CPU optimization..."))ha_cpu_unavailable(s);return 1;}
  s->cpu_choice=choice;
  if(query){if(!strcmp(s->notice,"Checking CPU optimization..."))s->notice[0]=0;return 1;}
  if(!strcmp(message,"saved"))snprintf(s->notice,sizeof s->notice,"CPU optimization: %s",ha_cpu_value(s));
  else snprintf(s->notice,sizeof s->notice,"CPU optimization: %s, %.90s",ha_cpu_value(s),message);
  return 1;
 }
 /* A global observation cannot acknowledge a timed-out request. Reopening
  * explicitly refreshes the confirmed current state without sending On. */
 if(s->cpu_uncertain)return changed;
 /* Keep this page's private confirmation against unordered/stale global
  * On/Off observations. A global unknown may still invalidate it safely. */
 if(s->cpu_confirmed){
  if(ha_read_line(UB_CPU_STATUS,text,sizeof text)&&!ha_cpu_status(text,&seq,&choice,&known,&message)&&!known){
   s->cpu_known=0;s->cpu_confirmed=0;s->cpu_uncertain=1;ha_copy(s->cpu_message,message,sizeof s->cpu_message);
   if(!s->notice[0]||!strncmp(s->notice,"CPU optimization",16))ha_cpu_unavailable(s);
   return 1;
  }
  return changed;
 }
 if(!ha_read_line(UB_CPU_STATUS,text,sizeof text)||ha_cpu_status(text,&seq,&choice,&known,&message)){
  changed|=s->cpu_known;s->cpu_known=0;ha_copy(s->cpu_message,"status unavailable; reopen Helper to refresh",sizeof s->cpu_message);return changed;
 }
 changed|=known!=s->cpu_known||(known&&choice!=s->cpu_choice);
 s->cpu_known=known;if(known)s->cpu_choice=choice;ha_copy(s->cpu_message,message,sizeof s->cpu_message);return changed;
}
static void ha_cpu_set(struct helper_state *s,unsigned int next){
 char text[32];int n;
 if(!s->cpu_enabled||!s->cpu_known){ha_cpu_unavailable(s);return;}
 if(s->cpu_wait){ha_copy(s->notice,"CPU request already running",sizeof s->notice);return;}
 /* The saved preference can differ from the live state after a suppressed or
  * failed startup. Even choosing the current state must reach the backend so
  * it can persist that explicit choice without repeating native activation. */
 if(next>1)return;
 if(!++s->cpu_counter)s->cpu_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",next?"on":"off",s->cpu_counter);
 if(ha_action_send(s,UB_CPU_REQUEST,text,(size_t)n,s->cpu_counter,&s->cpu_deadline)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->cpu_seq=s->cpu_counter;s->cpu_wait=1;s->cpu_query=0;ha_copy(s->notice,next?"Enabling CPU optimization...":"Disabling CPU optimization...",sizeof s->notice);
}
/* One explicit page-open query; the regular UI status poll never sends it. */
static void ha_cpu_refresh(struct helper_state *s){
 char text[32];int n;
 if(!s->cpu_enabled||s->cpu_wait)return;
 s->cpu_known=0;s->cpu_confirmed=0;s->cpu_uncertain=0;s->cpu_query=0;
 if(!++s->cpu_counter)s->cpu_counter=1;
 n=snprintf(text,sizeof text,"get %u\n",s->cpu_counter);
 if(ha_action_send(s,UB_CPU_REQUEST,text,(size_t)n,s->cpu_counter,&s->cpu_deadline)){
  s->cpu_uncertain=1;ha_copy(s->cpu_message,"cannot query current state; reopen Helper",sizeof s->cpu_message);ha_cpu_unavailable(s);return;
 }
 s->cpu_seq=s->cpu_counter;s->cpu_wait=1;s->cpu_query=1;
 ha_copy(s->cpu_message,"checking current state",sizeof s->cpu_message);ha_copy(s->notice,"Checking CPU optimization...",sizeof s->notice);
}

/* ---- Blu-ray region ---- */
/* Auto is last and listed only when the region module allows it (bd_auto). */
static const char *const ha_region_names[]={"stock","A","B","C","ABC","auto"};
static const char *const ha_region_labels[]={"Player default","A","B","C","A+B+C","Auto"};
#define HA_REGION_CHOICES 6u
#define HA_REGION_AUTO 5u
static unsigned int ha_region_count(const struct helper_state *s){return s->region_auto?HA_REGION_CHOICES:HA_REGION_CHOICES-1;}
static unsigned int ha_region_index(const char *name){unsigned int i;for(i=0;i<HA_REGION_CHOICES;i++)if(!strcmp(name,ha_region_names[i]))return i;return 0;}
static const char *ha_region_label(const struct helper_state *s){return s->region_known?ha_region_labels[ha_region_index(s->region_choice)]:"-";}
static const char *ha_disc_letters(unsigned int v){return v==1?"A":v==2?"B":v==4?"C":v==7?"A+B+C":"";}
/* "<seq> <choice> <disc> <player> <auto> <message>" from the region module; 1 if
 * shown state changed. */
static int ha_region_poll(struct helper_state *s){
 int answered=0,reply;
 char buf[160],choice[8],before[8];const char *p=buf;unsigned int seq,disc,player,autok,i=0,disc_before=s->region_disc;int known=s->region_known,auto_before=s->region_auto;
 ha_copy(before,s->region_choice,sizeof before);
 if(s->region_wait){
  reply=ha_action_poll(s,UB_REGION_REQUEST,s->region_seq,s->region_deadline,buf,sizeof buf);
  if(reply<0){s->region_wait=0;ha_copy(s->notice,"No confirmation from helper; result unknown",sizeof s->notice);return 1;}
  answered=reply==1;
 }
 if(!answered&&!ha_read_line(UB_REGION_STATUS,buf,sizeof buf)){s->region_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 while(*p&&*p!=' '&&i+1<sizeof choice)choice[i++]=*p++;
 choice[i]=0;if(*p++!=' ')return 0;
 disc=ha_parse(&p);if(*p++!=' ')return 0;
 player=ha_parse(&p);if(*p++!=' ')return 0;
 autok=ha_parse(&p);if(*p==' ')p++;
 s->region_known=1;s->region_disc=disc;s->region_player=player;s->region_auto=autok==1;
 if(!s->region_wait||(answered&&seq==s->region_seq))ha_copy(s->region_choice,choice,sizeof s->region_choice);
 if(s->region_wait&&(answered&&seq==s->region_seq)){
  s->region_wait=0;
  if(!strcmp(p,"saved"))snprintf(s->notice,sizeof s->notice,"Blu-ray region: %s. Takes effect with the next disc.",ha_region_label(s));
  else snprintf(s->notice,sizeof s->notice,"Blu-ray region: %s, %s",ha_region_label(s),p);
  return 1;
 }
 return !known||disc!=disc_before||auto_before!=s->region_auto||strcmp(before,s->region_choice);
}
/* The region value of the row, in words about what the player does (the
 * disc's own regions are not known): "Auto (player's own: B)", "Auto (set
 * to A for this disc)", "A (this disc still: B)" until the next disc. */
static const char *ha_region_value(const struct helper_state *s,char *buf,size_t cap){
 unsigned int k=ha_region_index(s->region_choice);const char *now=ha_disc_letters(s->region_disc),*own=ha_disc_letters(s->region_player);
 if(!s->region_known)return "not available";
 if(!now[0])ha_copy(buf,ha_region_labels[k],cap);
 else if(k==HA_REGION_AUTO&&s->region_disc==s->region_player)snprintf(buf,cap,"Auto (player's own: %s)",now);
 else if(k==HA_REGION_AUTO)snprintf(buf,cap,"Auto (set to %s for this disc)",now);
 else if(k==0&&s->region_disc==s->region_player)snprintf(buf,cap,"%s (%s)",ha_region_labels[k],own[0]?own:now);
 else if(strcmp(now,ha_region_labels[k]))snprintf(buf,cap,"%s (this disc still: %s)",ha_region_labels[k],now);
 else ha_copy(buf,ha_region_labels[k],cap);
 return buf;
}
/* Chooses region choice k (index into ha_region_names); nothing is sent when
 * it is already the current one. */
static void ha_region_set(struct helper_state *s,unsigned int next){
 char text[48];int n;
 if(!s->region_known){ha_copy(s->notice,"Needs the region module ([region] in modules.cfg)",sizeof s->notice);return;}
 if(s->region_wait){ha_copy(s->notice,"Saving...",sizeof s->notice);return;}
 if(next>=ha_region_count(s)||next==ha_region_index(s->region_choice))return;
 if(!++s->region_counter)s->region_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",ha_region_names[next],s->region_counter);
 if(ha_action_send(s,UB_REGION_REQUEST,text,(size_t)n,s->region_counter,&s->region_deadline)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 ha_copy(s->region_choice,ha_region_names[next],sizeof s->region_choice); /* the helper confirms */
 s->region_seq=s->region_counter;s->region_wait=1;ha_copy(s->notice,"Saving...",sizeof s->notice);
}

/* ---- DVD region ---- */
static const char *const ha_dvd_names[]={"stock","auto","free","1","2","3","4","5","6","7","8"};
static const char *const ha_dvd_labels[]={"Player default","Auto","No region check","1","2","3","4","5","6","7","8"};
#define HA_DVD_CHOICES 11u
static unsigned int ha_dvd_index(const char *name,size_t n){unsigned int i;for(i=0;i<HA_DVD_CHOICES;i++)if(strlen(ha_dvd_names[i])==n&&!strncmp(name,ha_dvd_names[i],n))return i;return 0;}
/* "<seq> <choice> <message>" from the region module; 1 if shown state
 * changed. */
static int ha_dvd_poll(struct helper_state *s){
 int answered=0,reply;
 char buf[160];const char *p=buf,*name;unsigned int seq,choice,before=s->dvd_choice;int known=s->dvd_known;
 if(s->dvd_wait){
  reply=ha_action_poll(s,UB_DVD_REQUEST,s->dvd_seq,s->dvd_deadline,buf,sizeof buf);
  if(reply<0){s->dvd_wait=0;ha_copy(s->notice,"No confirmation from helper; result unknown",sizeof s->notice);return 1;}
  answered=reply==1;
 }
 if(!answered&&!ha_read_line(UB_DVD_STATUS,buf,sizeof buf)){s->dvd_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 name=p;while(*p&&*p!=' ')p++;
 choice=ha_dvd_index(name,(size_t)(p-name));
 if(*p==' ')p++;
 s->dvd_known=1;
 if(!s->dvd_wait||(answered&&seq==s->dvd_seq))s->dvd_choice=choice;
 if(s->dvd_wait&&(answered&&seq==s->dvd_seq)){
  s->dvd_wait=0;
  if(!strcmp(p,"saved"))snprintf(s->notice,sizeof s->notice,"DVD region: %s. Restart a DVD that already stopped.",ha_dvd_labels[s->dvd_choice]);
  else snprintf(s->notice,sizeof s->notice,"DVD region: %s, %s",ha_dvd_labels[s->dvd_choice],p);
  return 1;
 }
 return !known||before!=s->dvd_choice;
}
/* Chooses k (index into ha_dvd_names); nothing is sent when it is already
 * the current one. */
static void ha_dvd_set(struct helper_state *s,unsigned int next){
 char text[48];int n;
 if(!s->dvd_known){ha_copy(s->notice,"Needs the region module ([region] in modules.cfg)",sizeof s->notice);return;}
 if(s->dvd_wait){ha_copy(s->notice,"Saving...",sizeof s->notice);return;}
 if(next>=HA_DVD_CHOICES||next==s->dvd_choice)return;
 if(!++s->dvd_counter)s->dvd_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",ha_dvd_names[next],s->dvd_counter);
 if(ha_action_send(s,UB_DVD_REQUEST,text,(size_t)n,s->dvd_counter,&s->dvd_deadline)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->dvd_choice=next; /* the helper confirms */
 s->dvd_seq=s->dvd_counter;s->dvd_wait=1;ha_copy(s->notice,"Saving...",sizeof s->notice);
}

/* ---- Netflix button ---- */
static const char *const ha_netflix_names[]={"netflix","nothing","helper"};
static const char *const ha_netflix_labels[]={"Netflix","Does nothing","UB820 Helper"};
#define HA_NETFLIX_CHOICES 3u
/* "<seq> <choice> <message>" from the remote module; 1 if shown state
 * changed. */
static int ha_netflix_poll(struct helper_state *s){
 int answered=0,reply;
 char buf[160];const char *p=buf;unsigned int seq,choice,before=s->netflix_choice;int known=s->netflix_known;
 if(s->netflix_wait){
  reply=ha_action_poll(s,UB_REMOTE_REQUEST,s->netflix_seq,s->netflix_deadline,buf,sizeof buf);
  if(reply<0){s->netflix_wait=0;ha_copy(s->notice,"No confirmation from helper; result unknown",sizeof s->notice);return 1;}
  answered=reply==1;
 }
 if(!answered&&!ha_read_line(UB_REMOTE_STATUS,buf,sizeof buf)){s->netflix_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 choice=!strncmp(p,"nothing",7)?1u:!strncmp(p,"helper",6)?2u:0u;
 while(*p&&*p!=' ')p++;
 if(*p==' ')p++;
 s->netflix_known=1;
 if(!s->netflix_wait||(answered&&seq==s->netflix_seq))s->netflix_choice=choice;
 if(s->netflix_wait&&(answered&&seq==s->netflix_seq)){
  s->netflix_wait=0;
  if(!strcmp(p,"saved"))snprintf(s->notice,sizeof s->notice,"Netflix button: %s",ha_netflix_labels[s->netflix_choice]);
  else snprintf(s->notice,sizeof s->notice,"Netflix button: %s, %s",ha_netflix_labels[s->netflix_choice],p);
  return 1;
 }
 return !known||before!=s->netflix_choice;
}
/* Chooses k (index into ha_netflix_names); nothing is sent when it is
 * already the current one. */
static void ha_netflix_set(struct helper_state *s,unsigned int next){
 char text[48];int n;
 if(!s->netflix_known){ha_copy(s->notice,"Needs the remote module ([remote] in modules.cfg)",sizeof s->notice);return;}
 if(s->netflix_wait){ha_copy(s->notice,"Saving...",sizeof s->notice);return;}
 if(next>=HA_NETFLIX_CHOICES||next==s->netflix_choice)return;
 if(!++s->netflix_counter)s->netflix_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",ha_netflix_names[next],s->netflix_counter);
 if(ha_action_send(s,UB_REMOTE_REQUEST,text,(size_t)n,s->netflix_counter,&s->netflix_deadline)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->netflix_choice=next; /* the helper confirms */
 s->netflix_seq=s->netflix_counter;s->netflix_wait=1;ha_copy(s->notice,"Saving...",sizeof s->notice);
}

/* ---- SSH ---- */
static void ha_ssh_load(struct helper_state *s){char buf[200];if(ha_read_line(UB_SSH_STATUS,buf,sizeof buf))s->ssh_running=buf[0]=='1';}
static void ha_ssh_toggle(struct helper_state *s){
 const char *request=s->ssh_running?"stop\n":"start\n";
 if(s->ssh_wait){ha_copy(s->notice,"SSH request already running",sizeof s->notice);return;}
 if(!++s->ssh_counter)s->ssh_counter=1;
 if(ha_action_send_timeout(s,UB_SSH_REQUEST,request,strlen(request),s->ssh_counter,&s->ssh_deadline,45u)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->ssh_seq=s->ssh_counter;s->ssh_wait=1;ha_copy(s->notice,s->ssh_running?"Stopping SSH...":"Starting SSH...",sizeof s->notice);
}
/* "<running> <code> <text>"; 1 when answered. Call about four times a second. */
static int ha_ssh_poll(struct helper_state *s){
 char buf[200];const char *p=buf;int reply;
 if(!s->ssh_wait)return 0;
 reply=ha_action_poll(s,UB_SSH_REQUEST,s->ssh_seq,s->ssh_deadline,buf,sizeof buf);
 if(!reply)return 0;
 if(reply<0){s->ssh_wait=0;ha_copy(s->notice,"No SSH confirmation; check status before retrying",sizeof s->notice);return 1;}
 s->ssh_running=buf[0]=='1';
 ha_parse(&p);if(*p==' ')p++;
 if(*p=='-')p++;
 ha_parse(&p);if(*p==' ')p++;
 s->ssh_wait=0;snprintf(s->notice,sizeof s->notice,"SSH: %s",p);return 1;
}

/* ---- Save log to USB ---- */
static void ha_log_save(struct helper_state *s){
 char text[32];int n;
 if(s->log_always){ha_copy(s->notice,"The log is already saved to USB a few seconds after each change",sizeof s->notice);return;}
 if(s->save_seq){ha_copy(s->notice,"Saving the log is already running",sizeof s->notice);return;}
 if(!++s->save_counter)s->save_counter=1;
 n=snprintf(text,sizeof text,"save-log %u\n",s->save_counter);
 if(ha_action_send_timeout(s,UB_STORAGE_REQUEST,text,(size_t)n,s->save_counter,&s->save_deadline,45u)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->save_seq=s->save_counter;s->save_ticks=0;ha_copy(s->notice,"Saving the log to USB...",sizeof s->notice);
}
/* 1 when answered. Call about four times a second. */
static int ha_log_poll(struct helper_state *s){
 char buf[160];const char *p=buf;int code,negative=0,reply;unsigned int seq;
 if(!s->save_seq)return 0;
 reply=ha_action_poll(s,UB_STORAGE_REQUEST,s->save_seq,s->save_deadline,buf,sizeof buf);
 if(!reply)return 0;
 if(reply<0){s->save_seq=0;ha_copy(s->notice,"No log-save confirmation; result unknown",sizeof s->notice);return 1;}
 seq=ha_parse(&p);if(*p++!=' '||seq!=s->save_seq)return 0;
 if(*p=='-'){negative=1;p++;}
 code=(int)ha_parse(&p);if(negative)code=-code;
 if(*p==' ')p++;
 s->save_seq=0;
 if(!code){
  if(!strncmp(p,"saved as ",9)||!strncmp(p,"saved to ",9))
   snprintf(s->notice,sizeof s->notice,"Log saved to USB: %s",p+9);
  else ha_copy(s->notice,*p?p:"Log saved to USB",sizeof s->notice);
 }
 else snprintf(s->notice,sizeof s->notice,"Saving failed: %s (code %d)",p,code);
 return 1;
}

/* Everything the page shows, read when it opens. */
static void ha_load(struct helper_state *s){
 char mode[16];
 if(s->ssh_wait)ha_action_cancel(s,UB_SSH_REQUEST,s->ssh_seq);
 if(s->save_seq)ha_action_cancel(s,UB_STORAGE_REQUEST,s->save_seq);
 if(s->region_wait)ha_action_cancel(s,UB_REGION_REQUEST,s->region_seq);
 if(s->dvd_wait)ha_action_cancel(s,UB_DVD_REQUEST,s->dvd_seq);
 if(s->netflix_wait)ha_action_cancel(s,UB_REMOTE_REQUEST,s->netflix_seq);
 if(s->power_wait)ha_action_cancel(s,UB_POWER_REQUEST,s->power_seq);
 if(s->cpu_wait)ha_action_cancel(s,UB_CPU_REQUEST,s->cpu_seq);
 s->notice[0]=0;s->ssh_wait=0;s->region_wait=0;s->netflix_wait=0;s->dvd_wait=0;s->save_seq=0;s->power_wait=0;s->power_uncertain=0;s->cpu_wait=0;s->cpu_uncertain=0;s->cpu_query=0;s->cpu_confirmed=0;
 s->ssh_enabled=ha_exists(HA_SSH_ENABLED);
 s->log_always=ha_read_line(UB_LOG_MODE,mode,sizeof mode)&&!strcmp(mode,"always");
 ha_ssh_load(s);ha_region_poll(s);ha_dvd_poll(s);ha_netflix_poll(s);ha_power_poll(s);ha_cpu_poll(s);ha_cpu_refresh(s);
}
#endif
