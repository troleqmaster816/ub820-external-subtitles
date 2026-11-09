#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../modules/external_subtitles/native_text.c"

static int empty_translation=1,null_translation,empty_english,set_calls,original_calls,plt_calls;
static unsigned int seen_language,seen_id;
static ub_gettext_callback callback;
const char *dgettext_ppro2(unsigned int language,unsigned int id){
 seen_language=language;seen_id=id;
 if(language==0x20)return empty_english?"":"English subtitle text";
 if(null_translation)return 0;
 return empty_translation?"":"Existing translation";
}
void CTK_util_set_gettextfunc(ub_gettext_callback value){set_calls++;callback=value;}
static const char *foreign(unsigned int language,unsigned int id){(void)language;(void)id;return "Foreign callback";}
static const char *stock_plt(unsigned int language,unsigned int id){plt_calls++;return dgettext_ppro2(language,id);}
static void original(void){original_calls++;assert(callback!=dgettext_ppro2);}
int main(void){
 callback=dgettext_ppro2;ub_gettext_slot=&callback;ub_original_native_rows=original;
 ub_hook_native_rows();assert(set_calls==1 && original_calls==1 && callback==ub_native_english_gettext);
 ub_hook_native_rows();assert(set_calls==1 && original_calls==2);
 for(unsigned int id=0x12b5;id<=0x12c0;id++){
  assert(!strcmp(callback(1,id),"English subtitle text"));assert(seen_language==0x20 && seen_id==id);
 }
 assert(!*callback(1,0x12b4));assert(seen_language==1);
 assert(!*callback(1,0x12c1));assert(seen_language==1);
 empty_translation=0;assert(!strcmp(callback(2,0x12b5),"Existing translation"));assert(seen_language==2);
 empty_translation=1;empty_english=1;assert(!*callback(2,0x12b5));
 null_translation=1;assert(!callback(99,0x12b5));assert(seen_language==99);
 callback=foreign;ub_hook_native_rows();assert(callback==foreign && set_calls==1 && original_calls==3);
 callback=0;ub_hook_native_rows();assert(!callback && set_calls==1 && original_calls==4);
 /* Actual stock registers a canonical ARM PLT pointer, not the export. */
 null_translation=empty_english=0;ub_stock_gettext_callback=stock_plt;callback=stock_plt;
 ub_hook_native_rows();assert(callback==ub_native_english_gettext && set_calls==2 && original_calls==5);
 assert(!strcmp(callback(1,0x12b5),"English subtitle text"));assert(plt_calls==1);
 ub_hook_native_rows();assert(set_calls==2 && original_calls==6);
 puts("native_text_test passed: narrow fallback; existing/null translations preserved; idempotent/foreign callback handling");
 return 0;
}
