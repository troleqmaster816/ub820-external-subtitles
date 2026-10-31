/* Host test of the external_subtitles English-text page builder on the real 1.82
 * apl_lang.dat (path in argv[1]): writes the original and patched pages to
 * argv[2] for run_tests.py to decode independently. */
#define _GNU_SOURCE
#include "ub.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../modules/external_subtitles/english_text.h"
struct ub_module *const ub_modules[]={0};

int main(int argc,char **argv){
 FILE *f;int rc;
 if(argc<3)return 2;
 ub_log_open();
 rc=lang_build(argv[1]);
 if(rc!=1){printf("lang_build=%d\n",rc);return 1;}
 f=fopen(argv[2],"wb");fwrite(lang_old,1,LANG_PAGES,f);fwrite(lang_new,1,LANG_PAGES,f);fclose(f);
 lang_state=0;
 if(lang_build("/dev/null")!=-1){puts("wrong file accepted");return 1;}
 puts("lang_test built pages");
 return 0;
}
