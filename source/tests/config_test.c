/* Host test: the real module set reads the shipped USB-CONTENTS/sub/modules.cfg
 * without a single config warning. Built with every modules/<id>/module.c and
 * the generated sources by tests/run_tests.py. */
#include "ub.h"
#include <stdio.h>
#include <string.h>

int main(void){
 char buf[8192];FILE *f;size_t n;int failures=0;
 ub_log_open();ub_config_load();
 f=fopen(UB_LOG_FILE,"r");n=f?fread(buf,1,sizeof buf-1,f):0;if(f)fclose(f);
 buf[n]=0;
 const int read_ok=strstr(buf,"/modules.cfg (")!=0; /* "config: read <dir>/modules.cfg (N lines)" */
 for(char *line=buf;line&&*line;){
  char *next=strchr(line,'\n');if(next)*next++=0;
  if(strstr(line,"config:")&&!strstr(line,"config: read ")){printf("config warning: %s\n",line);failures++;}
  line=next;
 }
 if(!read_ok){puts("FAIL: modules.cfg not read");failures++;}
 for(struct ub_module *const *mm=ub_modules;*mm;mm++){
  printf("[%s] %s",(*mm)->id,(*mm)->enabled?"on":"off");
  for(const struct ub_option *o=(*mm)->options;o&&o->key;o++)printf(" %s=%d",o->key,*o->value);
  putchar('\n');
  /* ssh is user-configurable (off by default); every other module ships on. */
  if(!(*mm)->enabled&&strcmp((*mm)->id,"ssh")){printf("FAIL: %s is off in the shipped config\n",(*mm)->id);failures++;}
 }
 printf("%s (%d failures)\n",failures?"config_test FAILED":"config_test passed",failures);
 return failures!=0;
}
