#define _GNU_SOURCE
#include "ub.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef UB_HOST_TEST
#include "storage.h"
#endif

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
/* 0 applied, -1 unknown module/key or invalid value. */
static int apply(const char *id,const char *key,const char *text,const char *source,unsigned line){
 struct ub_module *m=find_module(id);long v;
 if(!m){ub_log("config",UB_INFO,"%s:%u: unknown module [%s]",source,line,id);return -1;}
 if(!strcmp(key,"enabled")){
  if(parse_value(text,0,&v)||v<0||v>1){ub_log("config",UB_INFO,"%s:%u: [%s] enabled must be 0 or 1",source,line,id);return -1;}
  m->enabled=(int)v;return 0;
 }
 for(const struct ub_option *o=m->options;o&&o->key;o++){
  if(strcmp(o->key,key))continue;
  if(parse_value(text,o,&v)||v<o->min||v>o->max){ub_log("config",UB_INFO,"%s:%u: [%s] %s=%s out of range %d..%d; default kept",source,line,id,key,text,o->min,o->max);return -1;}
  *o->value=(int)v;return 0;
 }
 ub_log("config",UB_INFO,"%s:%u: [%s] has no option %s",source,line,id,key);return -1;
}
/* Choices made in the menu, kept so that saving one keeps the others. */
#define UB_MENU_FILE "ub820_menu.cfg"
static struct { char module[32],key[32],value[32]; } chosen[16];
static unsigned chosen_count;
static void remember(const char *module,const char *key,const char *value){
 unsigned i;
 for(i=0;i<chosen_count;i++)if(!strcmp(chosen[i].module,module)&&!strcmp(chosen[i].key,key))break;
 if(i==chosen_count){if(chosen_count==sizeof chosen/sizeof *chosen)return;chosen_count++;}
 snprintf(chosen[i].module,sizeof chosen[i].module,"%.31s",module);
 snprintf(chosen[i].key,sizeof chosen[i].key,"%.31s",key);
 snprintf(chosen[i].value,sizeof chosen[i].value,"%.31s",value);
}
static char *trim(char *s){
 char *e;while(isspace((unsigned char)*s))s++;
 e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;
 return s;
}
/* 0 read, -1 not readable (errno kept). menu: the entries are menu choices. */
static int parse_file(const char *path,int legacy_keys,int menu){
 static char stream[4096];
 char buf[256],section[64]="";unsigned line=0;FILE *f=fopen(path,"r");
 if(!f)return -1;
 setvbuf(f,stream,_IOFBF,sizeof stream); /* else the first read fstats (statx, see sys.h) */
 while(line<UB_CONFIG_LINES&&fgets(buf,sizeof buf,f)){
  char *p,*eq,*key,*value,*hash;
  line++;
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
   if(i<sizeof legacy/sizeof *legacy)apply(legacy[i].module,legacy[i].option,value,path,line);
   continue;
  }
  if(!section[0]){ub_log("config",UB_INFO,"%s:%u: %s outside a [module] section",path,line,key);continue;}
  if(!apply(section,key,value,path,line)&&menu)remember(section,key,value);
 }
 fclose(f);
 ub_log("config",UB_ALWAYS,"read %s (%u lines)",path,line);
 return 0;
}
#ifndef UB_HOST_TEST
static void fetch(const char *name,int optional){
 char from[128],to[128];int rc;
 snprintf(from,sizeof from,UB_STICK_DIR "/%s",name);snprintf(to,sizeof to,UB_CONFIG_DIR "/%s",name);
 unlink(to);
 rc=optional?ub_stick_read_quiet(from,to):ub_stick_read(from,to);
 if(!rc)ub_log("config",UB_INFO,"copied %s from the stick",from);
 else if(!(optional&&rc==4))ub_log("config",UB_ALWAYS,"cannot copy %s from the stick: %s (code %d)",from,ub_stick_error(rc),rc);
}
#endif
/* Without a readable modules.cfg every module runs with its defaults; the log
 * says so. */
void ub_config_load(void){
#ifndef UB_HOST_TEST
 mkdir(UB_CONFIG_DIR,0700);
 fetch("modules.cfg",0);fetch("disc_style.cfg",1);fetch(UB_MENU_FILE,1);
#endif
 int legacy=parse_file(UB_CONFIG_DIR "/disc_style.cfg",1,0);
 int legacy_errno=errno;
 if(parse_file(UB_CONFIG_DIR "/modules.cfg",0,0)&&legacy){
  int e=errno;
  ub_log("config",UB_ALWAYS,"no modules.cfg (%s, errno %d)%s; defaults used",strerror(e),e,legacy_errno==ENOENT?"":", nor disc_style.cfg");
 }
 parse_file(UB_CONFIG_DIR "/" UB_MENU_FILE,0,1);
}

/* A choice made in the menu: applied now and saved to sub/ub820_menu.cfg on
 * the stick together with every other menu choice, so it holds after a power
 * cycle. 0 saved; -1 invalid (nothing changed); >0 applied but not saved
 * (ub_stick_error code). */
int ub_setting_set(const char *module,const char *key,const char *value){
 static const char path[]=UB_CONFIG_DIR "/" UB_MENU_FILE;FILE *f;
 if(apply(module,key,value,"menu",0))return -1;
 remember(module,key,value);
 mkdir(UB_CONFIG_DIR,0700);
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
 if(fclose(f))return 5;
#ifndef UB_HOST_TEST
 return ub_stick_write(path,UB_STICK_DIR "/" UB_MENU_FILE);
#else
 return 0;
#endif
}
