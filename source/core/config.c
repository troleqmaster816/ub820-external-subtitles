#define _GNU_SOURCE
#include "ub.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "storage.h"

/* sub/modules.cfg:
 *   [module_id]
 *   enabled=1
 *   key=value      (integer, or a name the option lists)
 * The older sub/disc_style.cfg keys are still read first, so existing sticks
 * keep their settings; modules.cfg wins where both set a value.
 * sub/ub820_menu.cfg (same format) holds what was chosen in the UB820 Helper
 * menu; it is written by the helper (ub_setting_set) and read last, so a
 * menu choice wins over modules.cfg.
 * The stick is reached only through the player's file layer, so both files
 * are first copied from sub/ on the stick into UB_CONFIG_DIR. */
#ifndef UB_CONFIG_DIR
#define UB_CONFIG_DIR "/tmp/ub820_config"
#endif
#define UB_CONFIG_LINES 256

static const struct { const char *key,*module,*option; } legacy[]={
 {"font_percent","external_subtitles","font_percent"},
 {"position","external_subtitles","position"},
 {"bottom_align","external_subtitles","bottom_align"},
 {"native_options","external_subtitles","native_options"},
 {"english_fallback","external_subtitles","english_text"},
 {"usb_log","log","save"},
};

static struct ub_module *find_module(const char *id){
 for(struct ub_module *const *m=ub_modules;*m;m++)if(!strcmp((*m)->id,id))return *m;
 return 0;
}
int ub_module_enabled(const char *id){struct ub_module *m=find_module(id);return m&&m->enabled;}

static int parse_value(const char *text,const struct ub_option *o,long *out){
 char *end;long v;
 errno=0;v=strtol(text,&end,10);
 if(end!=text&&!errno&&!*end){*out=v;return 0;}
 if(o&&o->names)for(int i=0;o->names[i];i++)if(!strcasecmp(text,o->names[i])){*out=o->min+i;return 0;}
 return -1;
}
/* 0 applied (only checked when !set), -1 unknown module/key or invalid value. */
static int apply(const char *id,const char *key,const char *text,const char *source,unsigned line,int set,int *number){
 struct ub_module *m=find_module(id);long v;
 if(!m){ub_log("config",UB_INFO,"%s:%u: unknown module [%s]",source,line,id);return -1;}
 if(!strcmp(key,"enabled")){
  if(parse_value(text,0,&v)||v<0||v>1){ub_log("config",UB_INFO,"%s:%u: [%s] enabled must be 0 or 1",source,line,id);return -1;}
  if(set)m->enabled=(int)v;
  if(number)*number=(int)v;
  return 0;
 }
 for(const struct ub_option *o=m->options;o&&o->key;o++){
  if(strcmp(o->key,key))continue;
  if(parse_value(text,o,&v)||v<o->min||v>o->max){ub_log("config",UB_INFO,"%s:%u: [%s] %s=%s out of range %d..%d; default kept",source,line,id,key,text,o->min,o->max);return -1;}
  if(set)*o->value=(int)v;
  if(number)*number=(int)v;
  return 0;
 }
 ub_log("config",UB_INFO,"%s:%u: [%s] has no option %s",source,line,id,key);return -1;
}
/* Choices made in the menu, kept so that saving one keeps the others. */
#define UB_MENU_FILE "ub820_menu.cfg"
static struct { char module[32],key[32],value[32];int run; } chosen[64];
static unsigned chosen_count;
/* run: chosen in the menu during this run. A choice from the file never
 * replaces one made in this run; among the file's own entries the last one
 * wins, as when the file is applied. 0, or -1 when there is no room. */
static int remember(const char *module,const char *key,const char *value,int number,int run){
 unsigned i;
 for(i=0;i<chosen_count;i++)if(!strcmp(chosen[i].module,module)&&!strcmp(chosen[i].key,key))break;
 if(i<chosen_count&&chosen[i].run&&!run)return 0;
 if(i==chosen_count){if(chosen_count==sizeof chosen/sizeof *chosen)return -1;chosen_count++;}
 snprintf(chosen[i].module,sizeof chosen[i].module,"%.31s",module);
 snprintf(chosen[i].key,sizeof chosen[i].key,"%.31s",key);
 if(strlen(value)>=sizeof chosen[i].value)snprintf(chosen[i].value,sizeof chosen[i].value,"%d",number);
 else snprintf(chosen[i].value,sizeof chosen[i].value,"%s",value);
 chosen[i].run=run;return 0;
}
/* A rejected file contributes neither live values nor entries to the next
 * merge. Keep all choices made via the menu while the file was unreadable. */
static void keep_run_choices(void){
 unsigned kept=0;
 for(unsigned i=0;i<chosen_count;i++)if(chosen[i].run){
  if(kept!=i)chosen[kept]=chosen[i];
  kept++;
 }
 chosen_count=kept;
}
static char *trim(char *s){
 char *e;while(isspace((unsigned char)*s))s++;
 e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;
 return s;
}
/* 0 read, -1 not readable (errno kept). menu: the entries are menu choices,
 * 1 applied, 2 only merged into the saved set (they apply from the next start;
 * choices made in this run win); -2 the menu file was not read whole (over
 * UB_CONFIG_LINES lines or more choices than fit), so it must not be replaced
 * by what was read of it. */
static int parse_file(const char *path,int legacy_keys,int menu){
 static char stream[4096];
 char buf[256],section[64]="";unsigned line=0;int whole=1;FILE *f=fopen(path,"r");
 if(!f)return -1;
 if(menu)keep_run_choices();
 setvbuf(f,stream,_IOFBF,sizeof stream); /* else the first read fstats (statx, see sys.h) */
 while(line<UB_CONFIG_LINES&&fgets(buf,sizeof buf,f)){
  char *p,*eq,*key,*value,*hash;size_t length=strlen(buf);
  line++;
  /* A line longer than the buffer is ignored as a whole: its remainder must
   * never be read as a line of its own (it could look like key=value). */
  if(length&&buf[length-1]!='\n'&&!feof(f)){
   int c;while((c=getc(f))!=EOF&&c!='\n'){}
   ub_log("config",UB_ALWAYS,"%s:%u: line longer than %u characters ignored",path,line,(unsigned)sizeof buf-2);
   if(menu)whole=0;
   continue;
  }
  if((hash=strchr(buf,'#')))*hash=0;
  p=trim(buf);if(!*p)continue;
  if(*p=='['){
   char *close=strchr(p,']');
   if(!close||close[1]||close-p-1<=0||(size_t)(close-p-1)>=sizeof section){ub_log("config",UB_INFO,"%s:%u: bad section",path,line);section[0]=0;continue;}
   memcpy(section,p+1,(size_t)(close-p-1));section[close-p-1]=0;continue;
  }
  if(!(eq=strchr(p,'='))){ub_log("config",UB_INFO,"%s:%u: expected key=value",path,line);continue;}
  *eq=0;key=trim(p);value=trim(eq+1);
  if(legacy_keys){
   unsigned i;for(i=0;i<sizeof legacy/sizeof *legacy;i++)if(!strcmp(key,legacy[i].key))break;
   if(i<sizeof legacy/sizeof *legacy)apply(legacy[i].module,legacy[i].option,value,path,line,1,0);
   continue;
  }
  if(!section[0]){ub_log("config",UB_INFO,"%s:%u: %s outside a [module] section",path,line,key);continue;}
  int number;
  if(!apply(section,key,value,path,line,!menu,&number)&&menu&&remember(section,key,value,number,0))whole=0;
 }
 if(line==UB_CONFIG_LINES&&fgets(buf,sizeof buf,f))whole=0;
 if(ferror(f)){
  int error=errno?errno:EIO;
  if(menu)keep_run_choices();
  fclose(f);ub_log("config",UB_ALWAYS,"%s: read failed after %u lines (%s)",path,line,strerror(error));
  errno=error;return -1;
 }
 fclose(f);
 if(!whole){
  if(menu)ub_log("config",UB_ALWAYS,"%s: saved choices exceed parser limits; entire file ignored and not rewritten",path);
  else ub_log("config",UB_ALWAYS,"%s: only the first %u lines are read",path,line);
  if(menu){keep_run_choices();errno=EFBIG;return -2;}
 }
 if(menu==1)for(unsigned i=0;i<chosen_count;i++)
  apply(chosen[i].module,chosen[i].key,chosen[i].value,path,0,1,0);
 ub_log("config",UB_ALWAYS,"read %s (%u lines)",path,line);
 return 0;
}
/* 1 while the content of the stick's ub820_menu.cfg is known: read, or shown
 * absent by a directory listing. A failed read (code 4 included) proves
 * nothing, so the file is then never replaced with only this run's choices. */
static int menu_known=1;
static int menu_known_at_start;
int ub_menu_known_at_start(void){return menu_known_at_start;}
#ifndef UB_HOST_TEST
/* saved: a file the helper writes (ub820_menu.cfg), read as ub_stick_load. */
static int fetch(const char *name,int optional,int saved){
 char from[128],to[128];int rc;
 snprintf(from,sizeof from,UB_STICK_DIR "/%s",name);snprintf(to,sizeof to,UB_CONFIG_DIR "/%s",name);
 unlink(to);
 rc=saved?ub_stick_load(from,to,optional):optional?ub_stick_read_quiet(from,to):ub_stick_read(from,to);
 if(!rc)ub_log("config",UB_INFO,"copied %s from the stick",from);
 else if(!(optional&&rc==UB_STICK_ABSENT))ub_log("config",UB_ALWAYS,"cannot copy %s from the stick: %s (code %d)",from,ub_stick_error(rc),rc);
 return rc;
}
#endif
/* Without a readable modules.cfg every module runs with its defaults; the log
 * says so. */
void ub_config_load(void){
 int menu_rc=0;
#ifndef UB_HOST_TEST
 mkdir(UB_CONFIG_DIR,0700);
 fetch("modules.cfg",0,0);fetch("disc_style.cfg",1,0);menu_rc=fetch(UB_MENU_FILE,1,1);
#endif
 int legacy=parse_file(UB_CONFIG_DIR "/disc_style.cfg",1,0);
 int legacy_errno=errno;
 if(parse_file(UB_CONFIG_DIR "/modules.cfg",0,0)&&legacy){
  int e=errno;
  ub_log("config",UB_ALWAYS,"no modules.cfg (%s, errno %d)%s; defaults used",strerror(e),e,legacy_errno==ENOENT?"":", nor disc_style.cfg");
 }
 menu_known=!menu_rc&&!parse_file(UB_CONFIG_DIR "/" UB_MENU_FILE,0,1);
#ifndef UB_HOST_TEST
 if(menu_rc==UB_STICK_ABSENT)menu_known=1;
 if(!menu_known)ub_log("config",UB_ALWAYS,"%s/%s could not be read; menu choices are kept in RAM until it can",UB_STICK_DIR,UB_MENU_FILE);
#else
 menu_known=1;
#endif
 menu_known_at_start=menu_known;
}

/* A choice made in the menu: applied now and saved to sub/ub820_menu.cfg on
 * the stick together with every other menu choice, so it holds after a power
 * cycle. 0 saved; -1 invalid (nothing changed); >0 applied but not saved
 * (ub_stick_error code; UB_STICK_UNREAD: the stick's file could not be read,
 * so it is left as it is and the choice waits in RAM for the next save). */
/* Applies and remembers a choice without writing the stick: several staged
 * choices are then saved together by one ub_setting_flush (one USB write).
 * 0 staged; -1 invalid (nothing changed); UB_STICK_UNREAD no room. */
int ub_setting_stage(const char *module,const char *key,const char *value){
 int number;
 if(apply(module,key,value,"menu",0,1,&number))return -1;
 if(remember(module,key,value,number,1))return UB_STICK_UNREAD; /* no room: the file is left as it is */
 return 0;
}
int ub_setting_set(const char *module,const char *key,const char *value){
 int rc=ub_setting_stage(module,key,value);
 return rc?rc:ub_setting_flush();
}
/* Saves every remembered choice to sub/ub820_menu.cfg (same results as
 * ub_setting_set after its value was applied). */
int ub_setting_flush(void){
 static const char path[]=UB_CONFIG_DIR "/" UB_MENU_FILE;FILE *f;
#ifndef UB_HOST_TEST
 int rc;
#endif
 mkdir(UB_CONFIG_DIR,0700);
#ifndef UB_HOST_TEST
 if(!menu_known){
  /* Read it again; its other choices join this run's, which win. */
  rc=fetch(UB_MENU_FILE,1,1);
  if(rc&&rc!=UB_STICK_ABSENT)return UB_STICK_UNREAD;
  if(!rc&&parse_file(path,0,2))return UB_STICK_UNREAD;
  menu_known=1;
  ub_log("config",UB_ALWAYS,"%s/%s %s; pending menu choices are saved",UB_STICK_DIR,UB_MENU_FILE,rc?"is not on the stick":"read again");
 }
#endif
 static char stream[2048];
 if(!(f=fopen(path,"w")))return 5;
 setvbuf(f,stream,_IOFBF,sizeof stream); /* no fstat (statx, see sys.h) */
 fputs("# Written by the UB820 Helper menu and read after modules.cfg, so these win.\n"
       "# Delete this file to use modules.cfg alone.\n",f);
 for(unsigned i=0;i<chosen_count;i++){
  unsigned j;
  for(j=0;j<i&&strcmp(chosen[j].module,chosen[i].module);j++){}
  if(j<i)continue; /* this module's section is already written */
  fprintf(f,"\n[%s]\n",chosen[i].module);
  for(j=i;j<chosen_count;j++)if(!strcmp(chosen[j].module,chosen[i].module))fprintf(f,"%s=%s\n",chosen[j].key,chosen[j].value);
 }
 /* A failed flush during fprintf discards that buffer and is not reported
  * by fclose: never hand a truncated file to the stick. */
 {int failed=ferror(f);if(fclose(f)||failed)return 5;}
#ifndef UB_HOST_TEST
 rc=ub_stick_write(path,UB_STICK_DIR "/" UB_MENU_FILE);
 return rc<0?UB_STICK_NO_TOOL:rc; /* never negative: the value is applied */
#else
 return 0;
#endif
}

/* Used for a prerequisite marker when the operation itself has not begun.
 * Restore the live and remembered choices on failure. A timeout can still
 * have reached USB; this does not claim to roll the stick back. */
int ub_setting_set_saved(const char *module,const char *key,const char *value){
 struct ub_module *m=find_module(module);int *live=0,old,rc,known=menu_known;
 unsigned count=chosen_count;unsigned char previous[sizeof chosen];
 if(!m)return -1;
 if(!strcmp(key,"enabled"))live=&m->enabled;
 else for(const struct ub_option *o=m->options;o&&o->key;o++)if(!strcmp(o->key,key)){live=o->value;break;}
 if(!live)return -1;
 old=*live;memcpy(previous,chosen,sizeof chosen);
 rc=ub_setting_set(module,key,value);
 if(rc){*live=old;memcpy(chosen,previous,sizeof chosen);chosen_count=count;menu_known=known;}
 return rc;
}
