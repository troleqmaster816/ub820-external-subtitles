/* Helper side of the subtitle memory (analysis/subtitle-memory-20261121,
 * architecture v2). Included by module.c after engage_core.c, style.h and
 * brightness_supervisor.h.
 *
 * - The player's own subtitle settings live in play_dat (8 bytes, RAM):
 *   0 position, 1 luminance, 2 size, 3 sync, 4 character code, 5 on.
 *   Position and size chosen in the player's menus are saved globally to
 *   sub/ub820_menu.cfg; sync and character code are per film (stage B).
 * - The last folder where a file was chosen is kept in
 *   sub/ub820_subtitles.cfg and handed to the next playback's browser.
 * Nothing here waits on the player: play_dat is read without its lock, and
 * every USB write goes through the existing storage tool. */
#ifndef UB_SUBTITLE_MEMORY_H
#define UB_SUBTITLE_MEMORY_H
#include "sub_memory_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UB_SUB_CONFIG_DIR
#define UB_SUB_CONFIG_DIR "/tmp/ub820_config"
#endif
#define UB_SUB_MEMORY_NAME "ub820_subtitles.cfg"
#define UB_SUB_MEMORY_MAX 200u
#define UB_SUB_MEMORY_LINE 320u
#define PREF_STABLE_SAMPLES 3u
#define SAVE_DEBOUNCE_MS 5000ull
#define SAVE_MIN_GAP_MS 15000ull
#define SAVE_RETRY_LONG_MS 60000ull

/* ---- play_dat observation (no I/O) ----------------------------------------
 * Armed only after this playback's own start write (base = the bytes it
 * stored) or, when the engine already ran without it, from the first stable
 * reading. A stock reset is never taken for a choice:
 *   - byte5 0: dispsrv eject/mode resets (all bytes 0) or subtitles off;
 *   - byte0 and byte1 both 0 at once while byte0 was a far value or byte1
 *     was set: apl_bdv's discEjectAck clears exactly these two.
 * Other changes count per field once they stay the same for
 * PREF_STABLE_SAMPLES stable readings with the engine loaded. */
enum { PREF_POSITION=0, PREF_SIZE=2, PREF_SYNC=3, PREF_CODE=4 };
struct prefs_obs {
 int armed,have_prev;
 unsigned char base[8],prev[8];
 unsigned char cand[5];unsigned count[5];
};
static void prefs_disarm(struct prefs_obs *o){memset(o,0,sizeof *o);}
static void prefs_arm(struct prefs_obs *o,const unsigned char bytes[8]){
 memset(o,0,sizeof *o);o->armed=1;memcpy(o->base,bytes,8);memcpy(o->prev,bytes,8);o->have_prev=1;
}
static void prefs_forget_candidates(struct prefs_obs *o){memset(o->count,0,sizeof o->count);}
static int prefs_stock_reset(const struct prefs_obs *o,const unsigned char s[8]){
 int previous=(signed char)o->prev[0];
 if(!s[5])return 1;
 return o->have_prev&&!s[0]&&!s[1]&&o->prev[0]&&(previous>1||previous<-1||o->prev[1]);
}
static int prefs_value_ok(unsigned field,unsigned char raw){
 signed char v=(signed char)raw;
 switch(field){
 case PREF_POSITION:return v>=-32&&v<=32;
 case PREF_SIZE:return raw<=2;
 case PREF_SYNC:return v>=-30&&v<=30;
 case PREF_CODE:return raw<=1;
 }
 return 0;
}
/* One stable reading. Returns a mask (1<<field) of fields that changed for
 * good; their new values are in o->base. */
static unsigned prefs_sample(struct prefs_obs *o,const unsigned char s[8],int loaded){
 static const unsigned fields[]={PREF_POSITION,PREF_SIZE,PREF_SYNC,PREF_CODE};
 unsigned changed=0;
 if(!o->armed)return 0;
 if(!loaded||prefs_stock_reset(o,s)){
  if(loaded)memcpy(o->base,s,8); /* a reset is the new reference, never a choice */
  prefs_forget_candidates(o);memcpy(o->prev,s,8);o->have_prev=1;return 0;
 }
 o->base[1]=s[1];o->base[5]=s[5];o->base[6]=s[6];o->base[7]=s[7];
 for(unsigned k=0;k<sizeof fields/sizeof *fields;k++){
  unsigned f=fields[k];
  if(s[f]==o->base[f]){o->count[f]=0;continue;}
  if(o->count[f]&&o->cand[f]==s[f])o->count[f]++;else{o->cand[f]=s[f];o->count[f]=1;}
  if(o->count[f]>=PREF_STABLE_SAMPLES){
   o->count[f]=0;o->base[f]=s[f];
   if(prefs_value_ok(f,s[f]))changed|=1u<<f;
  }
 }
 memcpy(o->prev,s,8);o->have_prev=1;
 return changed;
}

/* ---- sub/ub820_subtitles.cfg (no I/O) -------------------------------------
 *   last_dir=/mnt/usb/...
 *   disc=<32 hex> <charcode 0|1> <sync -30..30> <path to the end of the line>
 * disc lines are most recent first (stage B uses them; stage A keeps them).
 * Untrusted text: anything malformed is skipped and counted. */
struct sub_disc { char id[33]; int charcode,sync; char path[UB_SUB_PATH_BYTES]; };
struct sub_memory {
 int known;            /* stick content known (read, or listed as absent) */
 unsigned rejected;    /* malformed lines in the last parse */
 char last_dir[UB_SUB_PATH_BYTES];
 unsigned count;
 struct sub_disc disc[UB_SUB_MEMORY_MAX];
};
static int sub_srt_name(const char *s){
 size_t n=strlen(s);
 return n>4&&s[n-4]=='.'&&(s[n-3]|32)=='s'&&(s[n-2]|32)=='r'&&(s[n-1]|32)=='t';
}
static int sub_disc_id_ok(const char *s){
 for(unsigned i=0;i<32;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
 return !s[32];
}
static int sub_memory_line(struct sub_memory *m,char *line){
 if(!strncmp(line,"last_dir=",9)){
  char *p=line+9;
  if(strlen(p)>=UB_SUB_PATH_BYTES||!ub_sub_path_ok(p,strlen(p)+1))return -1;
  memcpy(m->last_dir,p,strlen(p)+1);return 0; /* length checked above */
 }
 if(!strncmp(line,"disc=",5)){
  struct sub_disc d;char *p=line+5,*end;long code,sync;
  memset(&d,0,sizeof d);
  if(strlen(p)<33||p[32]!=' ')return -1;
  memcpy(d.id,p,32);d.id[32]=0;if(!sub_disc_id_ok(d.id))return -1;
  p+=33;if(*p!='0'&&*p!='1')return -1;code=*p-'0';p++;if(*p++!=' ')return -1;
  sync=strtol(p,&end,10);if(end==p||*end!=' '||sync<-30||sync>30)return -1;
  p=end+1;
  if(strlen(p)>=UB_SUB_PATH_BYTES||!ub_sub_path_ok(p,strlen(p)+1)||!sub_srt_name(p))return -1;
  for(unsigned i=0;i<m->count;i++)if(!strcmp(m->disc[i].id,d.id))return 0; /* first (newest) wins */
  if(m->count>=UB_SUB_MEMORY_MAX)return 0;
  d.charcode=(int)code;d.sync=(int)sync;memcpy(d.path,p,strlen(p)+1);
  m->disc[m->count++]=d;return 0;
 }
 return -1;
}
/* Whole file text (NUL-terminated). Lines longer than UB_SUB_MEMORY_LINE
 * are rejected as a whole. */
static void sub_memory_parse(struct sub_memory *m,const char *text){
 const char *p=text;
 m->last_dir[0]=0;m->count=0;m->rejected=0;
 while(*p){
  const char *nl=strchr(p,'\n');size_t n=nl?(size_t)(nl-p):strlen(p);char line[UB_SUB_MEMORY_LINE+1];
  if(n>UB_SUB_MEMORY_LINE)m->rejected++;
  else{
   size_t used=n; /* n still advances p below */
   memcpy(line,p,n);line[n]=0;
   if(used&&line[used-1]=='\r')line[--used]=0;
   if(used&&line[0]!='#'&&sub_memory_line(m,line))m->rejected++;
  }
  p+=n;if(*p=='\n')p++;
 }
}
/* Bytes written (without NUL), or 0 when the buffer is too small. */
static size_t sub_memory_format(const struct sub_memory *m,char *out,size_t cap){
 size_t used=0;int n;
 n=snprintf(out,cap,"# UB820 Helper subtitle memory v1\n");if(n<0||(size_t)n>=cap)return 0;used=(size_t)n;
 if(m->last_dir[0]){n=snprintf(out+used,cap-used,"last_dir=%s\n",m->last_dir);if(n<0||(size_t)n>=cap-used)return 0;used+=(size_t)n;}
 for(unsigned i=0;i<m->count&&i<UB_SUB_MEMORY_MAX;i++){
  n=snprintf(out+used,cap-used,"disc=%s %d %d %s\n",m->disc[i].id,m->disc[i].charcode,m->disc[i].sync,m->disc[i].path);
  if(n<0||(size_t)n>=cap-used)return 0;
  used+=(size_t)n;
 }
 return used;
}
/* RAM was in effect while the stick's file was unreadable: the file's own
 * content joins it, this run's values win. */
static void sub_memory_merge(struct sub_memory *ram,const struct sub_memory *file){
 if(!ram->last_dir[0])snprintf(ram->last_dir,sizeof ram->last_dir,"%s",file->last_dir);
 for(unsigned i=0;i<file->count&&ram->count<UB_SUB_MEMORY_MAX;i++){
  unsigned j;for(j=0;j<ram->count&&strcmp(ram->disc[j].id,file->disc[i].id);j++){}
  if(j==ram->count)ram->disc[ram->count++]=file->disc[i];
 }
}
/* ---- disc memory (no I/O) -------------------------------------------------
 * id -> file, charcode, sync; most recent first, at most UB_SUB_MEMORY_MAX. */
static int sub_memory_find(const struct sub_memory *m,const char *id){
 for(unsigned i=0;i<m->count;i++)if(!strcmp(m->disc[i].id,id))return (int)i;
 return -1;
}
/* Entry i becomes the most recent. 1 when the order changed. */
static int sub_memory_front(struct sub_memory *m,unsigned i){
 struct sub_disc d;
 if(!i||i>=m->count)return 0;
 d=m->disc[i];memmove(&m->disc[1],&m->disc[0],i*sizeof *m->disc);m->disc[0]=d;
 return 1;
}
/* Sets id -> path/charcode/sync as the most recent entry; the oldest drops
 * out beyond the limit. 1 when anything changed, 0 unchanged, -1 refused. */
static int sub_memory_put(struct sub_memory *m,const char *id,const char *path,int charcode,int sync){
 struct sub_disc d;int i=sub_memory_find(m,id),changed;
 if(!sub_disc_id_ok(id)||strlen(path)>=UB_SUB_PATH_BYTES||!ub_sub_path_ok(path,strlen(path)+1)||!sub_srt_name(path)||
    charcode<0||charcode>1||sync<-30||sync>30)return -1;
 if(i>=0){
  struct sub_disc *e=&m->disc[i];
  changed=strcmp(e->path,path)||e->charcode!=charcode||e->sync!=sync;
  memcpy(e->path,path,strlen(path)+1);e->charcode=charcode;e->sync=sync;
  return sub_memory_front(m,(unsigned)i)|changed;
 }
 memset(&d,0,sizeof d);memcpy(d.id,id,33);memcpy(d.path,path,strlen(path)+1);d.charcode=charcode;d.sync=sync;
 if(m->count<UB_SUB_MEMORY_MAX)m->count++;
 memmove(&m->disc[1],&m->disc[0],(m->count-1)*sizeof *m->disc);m->disc[0]=d;
 return 1;
}
static int disc_sync(unsigned char raw){signed char v=(signed char)raw;return v>=-30&&v<=30?v:0;}
static int disc_code(unsigned char raw){return raw<=1?raw:0;}
/* A new selection record of this playback. kind 2 (chosen in the browser)
 * stores the file with the current play_dat sync/charcode; kind 3 (the
 * remembered file opened) only refreshes its entry; kind 1 (sub.srt) never
 * creates one. 1 when the memory changed. */
static int disc_learn_selection(struct sub_memory *m,const char *id,unsigned kind,const char *path,const unsigned char bytes[8]){
 int i;
 if(!id[0])return 0;
 if(kind==UB_SUB_KIND_CHOSEN)return sub_memory_put(m,id,path,disc_code(bytes[4]),disc_sync(bytes[3]))>0;
 if(kind!=UB_SUB_KIND_REMEMBERED)return 0;
 i=sub_memory_find(m,id);
 if(i<0||strcmp(m->disc[i].path,path))return sub_memory_put(m,id,path,disc_code(bytes[4]),disc_sync(bytes[3]))>0;
 return sub_memory_front(m,(unsigned)i);
}
/* Stable sync/charcode changes (prefs_sample mask, values in base) while the
 * file of this playback is one the memory follows (kind 2 or 3). */
static int disc_learn_prefs(struct sub_memory *m,const char *id,unsigned kind,unsigned mask,const unsigned char base[8]){
 int i,changed=0;
 if(!id[0]||(kind!=UB_SUB_KIND_CHOSEN&&kind!=UB_SUB_KIND_REMEMBERED))return 0;
 if((i=sub_memory_find(m,id))<0)return 0;
 if((mask&(1u<<PREF_SYNC))&&m->disc[i].sync!=disc_sync(base[PREF_SYNC])){m->disc[i].sync=disc_sync(base[PREF_SYNC]);changed=1;}
 if((mask&(1u<<PREF_CODE))&&m->disc[i].charcode!=disc_code(base[PREF_CODE])){m->disc[i].charcode=disc_code(base[PREF_CODE]);changed=1;}
 return changed;
}
/* After each new selection: the current sync/charcode are the reference
 * (a failed autoload's reset to 0/UTF-8 is no viewer's choice). */
static void prefs_rebase_file(struct prefs_obs *o,const unsigned char s[8]){
 if(!o->armed)return;
 o->base[PREF_SYNC]=s[PREF_SYNC];o->base[PREF_CODE]=s[PREF_CODE];o->count[PREF_SYNC]=o->count[PREF_CODE]=0;
}

/* "/mnt/usb/a/b.srt" -> "/mnt/usb/a". 0, or -1 when the result is no valid folder. */
static int sub_dirname(const char *path,char *out,size_t cap){
 const char *slash=strrchr(path,'/');size_t n;
 if(!slash)return -1;
 n=(size_t)(slash-path);if(n>=cap)return -1;
 memcpy(out,path,n);out[n]=0;
 return ub_sub_path_ok(out,cap)?0:-1;
}

/* ---- save scheduling (no I/O) ---------------------------------------------
 * At most one write per SAVE_MIN_GAP_MS, SAVE_DEBOUNCE_MS after the last
 * change (or at once when forced, e.g. playback ended). After a failure: 15 s,
 * then 60 s, then only on a new change or a forced moment. */
struct save_sched { int dirty,failures,held; unsigned long long last_change,next_ok; };
static void sched_changed(struct save_sched *s,unsigned long long now){s->dirty=1;s->held=0;s->last_change=now;}
static int sched_due(const struct save_sched *s,unsigned long long now,int forced){
 if(!s->dirty||now<s->next_ok)return 0;
 if(s->held&&!forced)return 0;
 return forced||now-s->last_change>=SAVE_DEBOUNCE_MS;
}
static void sched_result(struct save_sched *s,unsigned long long now,int ok){
 if(ok){s->dirty=0;s->failures=0;s->held=0;s->next_ok=now+SAVE_MIN_GAP_MS;return;}
 s->failures++;
 if(s->failures==1)s->next_ok=now+SAVE_MIN_GAP_MS;
 else if(s->failures==2)s->next_ok=now+SAVE_RETRY_LONG_MS;
 else{s->held=1;s->next_ok=now+SAVE_MIN_GAP_MS;}
}
#endif
