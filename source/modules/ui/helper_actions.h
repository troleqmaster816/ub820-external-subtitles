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

#define HA_SSH_ENABLED "/tmp/ub820_ssh.enabled"
struct helper_state {
 int ssh_enabled,ssh_running; unsigned int ssh_wait;
 int log_always; unsigned int save_seq,save_ticks,save_counter;
 int region_known; char region_choice[8]; unsigned int region_disc,region_player,region_seq,region_wait,region_counter;
 int netflix_known; unsigned int netflix_choice,netflix_seq,netflix_wait,netflix_counter;
 int dvd_known; unsigned int dvd_choice,dvd_seq,dvd_wait,dvd_counter;
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
/* Writes a request atomically (temp file + rename): 0 ok. */
static int ha_request(const char *path,const char *text,size_t n){
 char temp[96];int fd,ok;
 snprintf(temp,sizeof temp,"%s.ui-%ld",path,(long)getpid());
 fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
 ok=write(fd,text,n)==(ssize_t)n;if(close(fd))ok=0;
 if(ok&&!rename(temp,path))return 0;
 unlink(temp);return -1;
}
static unsigned int ha_parse(const char **p){unsigned int v=0;while(**p>='0'&&**p<='9')v=v*10+(unsigned int)(*(*p)++-'0');return v;}

/* ---- Blu-ray region ---- */
static const char *const ha_region_names[]={"auto","A","B","C","ABC","stock"};
static const char *const ha_region_labels[]={"Auto","A","B","C","A+B+C","Player default"};
#define HA_REGION_CHOICES 6u
static unsigned int ha_region_index(const char *name){unsigned int i;for(i=0;i<HA_REGION_CHOICES;i++)if(!strcmp(name,ha_region_names[i]))return i;return 0;}
static const char *ha_region_label(const struct helper_state *s){return s->region_known?ha_region_labels[ha_region_index(s->region_choice)]:"-";}
static const char *ha_disc_letters(unsigned int v){return v==1?"A":v==2?"B":v==4?"C":v==7?"A+B+C":"";}
/* "<seq> <choice> <disc> <player> <message>" from the region module; 1 if
 * shown state changed. */
static int ha_region_poll(struct helper_state *s){
 char buf[160],choice[8],before[8];const char *p=buf;unsigned int seq,disc,player,i=0,disc_before=s->region_disc;int known=s->region_known;
 ha_copy(before,s->region_choice,sizeof before);
 if(!ha_read_line(UB_REGION_STATUS,buf,sizeof buf)){s->region_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 while(*p&&*p!=' '&&i+1<sizeof choice)choice[i++]=*p++;
 choice[i]=0;if(*p++!=' ')return 0;
 disc=ha_parse(&p);if(*p++!=' ')return 0;
 player=ha_parse(&p);if(*p==' ')p++;
 s->region_known=1;s->region_disc=disc;s->region_player=player;
 if(!s->region_wait||seq==s->region_seq)ha_copy(s->region_choice,choice,sizeof s->region_choice);
 if(s->region_wait&&seq==s->region_seq){
  s->region_wait=0;
  if(!strcmp(p,"saved"))snprintf(s->notice,sizeof s->notice,"Blu-ray region: %s. Takes effect with the next disc.",ha_region_label(s));
  else snprintf(s->notice,sizeof s->notice,"Blu-ray region: %s, %s",ha_region_label(s),p);
  return 1;
 }
 return !known||disc!=disc_before||strcmp(before,s->region_choice);
}
/* The region value of the row, in words about what the player does (the
 * disc's own regions are not known): "Auto (player's own: B)", "Auto (set
 * to A for this disc)", "A (this disc still: B)" until the next disc. */
static const char *ha_region_value(const struct helper_state *s,char *buf,size_t cap){
 unsigned int k=ha_region_index(s->region_choice);const char *now=ha_disc_letters(s->region_disc),*own=ha_disc_letters(s->region_player);
 if(!s->region_known)return "not available";
 if(!now[0])ha_copy(buf,ha_region_labels[k],cap);
 else if(k==0&&s->region_disc==s->region_player)snprintf(buf,cap,"Auto (player's own: %s)",now);
 else if(k==0)snprintf(buf,cap,"Auto (set to %s for this disc)",now);
 else if(k==HA_REGION_CHOICES-1&&s->region_disc==s->region_player)snprintf(buf,cap,"%s (%s)",ha_region_labels[k],own[0]?own:now);
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
 if(next>=HA_REGION_CHOICES||next==ha_region_index(s->region_choice))return;
 if(!++s->region_counter)s->region_counter=1;
 n=snprintf(text,sizeof text,"set %s %u\n",ha_region_names[next],s->region_counter);
 if(ha_request(UB_REGION_REQUEST,text,(size_t)n)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
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
 char buf[160];const char *p=buf,*name;unsigned int seq,choice,before=s->dvd_choice;int known=s->dvd_known;
 if(!ha_read_line(UB_DVD_STATUS,buf,sizeof buf)){s->dvd_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 name=p;while(*p&&*p!=' ')p++;
 choice=ha_dvd_index(name,(size_t)(p-name));
 if(*p==' ')p++;
 s->dvd_known=1;
 if(!s->dvd_wait||seq==s->dvd_seq)s->dvd_choice=choice;
 if(s->dvd_wait&&seq==s->dvd_seq){
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
 if(ha_request(UB_DVD_REQUEST,text,(size_t)n)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
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
 char buf[160];const char *p=buf;unsigned int seq,choice,before=s->netflix_choice;int known=s->netflix_known;
 if(!ha_read_line(UB_REMOTE_STATUS,buf,sizeof buf)){s->netflix_known=0;return known;}
 seq=ha_parse(&p);if(*p++!=' ')return 0;
 choice=!strncmp(p,"nothing",7)?1u:!strncmp(p,"helper",6)?2u:0u;
 while(*p&&*p!=' ')p++;
 if(*p==' ')p++;
 s->netflix_known=1;
 if(!s->netflix_wait||seq==s->netflix_seq)s->netflix_choice=choice;
 if(s->netflix_wait&&seq==s->netflix_seq){
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
 if(ha_request(UB_REMOTE_REQUEST,text,(size_t)n)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->netflix_choice=next; /* the helper confirms */
 s->netflix_seq=s->netflix_counter;s->netflix_wait=1;ha_copy(s->notice,"Saving...",sizeof s->notice);
}

/* ---- SSH ---- */
static void ha_ssh_load(struct helper_state *s){char buf[200];if(ha_read_line(UB_SSH_STATUS,buf,sizeof buf))s->ssh_running=buf[0]=='1';}
static void ha_ssh_toggle(struct helper_state *s){
 static const char t[]="toggle\n";
 if(s->ssh_wait){ha_copy(s->notice,"SSH request already running",sizeof s->notice);return;}
 unlink(UB_SSH_STATUS);
 if(ha_request(UB_SSH_REQUEST,t,sizeof t-1)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->ssh_wait=1;ha_copy(s->notice,s->ssh_running?"Stopping SSH...":"Starting SSH...",sizeof s->notice);
}
/* "<running> <code> <text>"; 1 when answered. Call about four times a second. */
static int ha_ssh_poll(struct helper_state *s){
 char buf[200];const char *p=buf;
 if(!s->ssh_wait)return 0;
 if(++s->ssh_wait>150){s->ssh_wait=0;ha_copy(s->notice,"No answer; is [ssh] enabled in modules.cfg?",sizeof s->notice);return 1;}
 if(!ha_read_line(UB_SSH_STATUS,buf,sizeof buf))return 0;
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
 unlink(UB_STORAGE_STATUS);
 if(ha_request(UB_STORAGE_REQUEST,text,(size_t)n)){ha_copy(s->notice,"Cannot contact the helper",sizeof s->notice);return;}
 s->save_seq=s->save_counter;s->save_ticks=0;ha_copy(s->notice,"Saving the log to USB...",sizeof s->notice);
}
/* 1 when answered. Call about four times a second. */
static int ha_log_poll(struct helper_state *s){
 char buf[160];const char *p=buf;int code,negative=0;unsigned int seq;
 if(!s->save_seq)return 0;
 if(++s->save_ticks>120){s->save_seq=0;ha_copy(s->notice,"No answer from the helper; is [log] enabled?",sizeof s->notice);return 1;}
 if(!ha_read_line(UB_STORAGE_STATUS,buf,sizeof buf))return 0;
 seq=ha_parse(&p);if(*p++!=' '||seq!=s->save_seq)return 0;
 if(*p=='-'){negative=1;p++;}
 code=(int)ha_parse(&p);if(negative)code=-code;
 if(*p==' ')p++;
 s->save_seq=0;
 if(!code)ha_copy(s->notice,"Log saved to USB: sub/ub820_submenu.log",sizeof s->notice);
 else snprintf(s->notice,sizeof s->notice,"Saving failed: %s (code %d)",p,code);
 return 1;
}

/* Everything the page shows, read when it opens. */
static void ha_load(struct helper_state *s){
 char mode[16];
 s->notice[0]=0;s->ssh_wait=0;s->region_wait=0;s->netflix_wait=0;s->dvd_wait=0;s->save_seq=0;
 s->ssh_enabled=ha_exists(HA_SSH_ENABLED);
 s->log_always=ha_read_line(UB_LOG_MODE,mode,sizeof mode)&&!strcmp(mode,"always");
 ha_ssh_load(s);ha_region_poll(s);ha_dvd_poll(s);ha_netflix_poll(s);
}
#endif
