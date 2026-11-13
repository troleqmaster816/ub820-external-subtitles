#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../modules/external_subtitles/native_text.c"

static int null_translation,empty_translation=1,empty_english,null_english,calls;
static unsigned seen_column[4],seen_id[4],current_column;
static const char blank[]="",translation[]="Existing translation",english[]="English subtitle text";
static const char *native_lookup(unsigned column,unsigned id){
 assert(calls<4);seen_column[calls]=column;seen_id[calls++]=id;
 if(column==16)return null_english?0:empty_english?blank:english;
 return null_translation?0:empty_translation?blank:translation;
}
/* Native exports use this same lookup; CTK registration is not involved. */
static const char *direct_gettext(unsigned id){return ub_hook_native_lookup(current_column,id);}
static const char *explicit_gettext(unsigned domain,unsigned id){
 unsigned column=domain==0x20?16:domain;
 return ub_hook_native_lookup(column,id);
}
static void reset_calls(void){calls=0;}
int main(void){
 ub_original_native_lookup=native_lookup;
 for(unsigned column=0;column<16;column++)for(unsigned id=0x12b5;id<=0x12c0;id++){
  current_column=column;reset_calls();assert(direct_gettext(id)==english);
  assert(calls==2&&seen_column[0]==column&&seen_column[1]==16&&seen_id[0]==id&&seen_id[1]==id);
  reset_calls();assert(explicit_gettext(column,id)==english&&calls==2);
 }
 reset_calls();assert(explicit_gettext(0x20,0x12bc)==english&&calls==1&&seen_column[0]==16);
 for(unsigned i=0;i<4;i++){
  unsigned id=(unsigned[]){0x12b4,0x12c1,0xbb7,0xb02}[i];
  reset_calls();assert(ub_hook_native_lookup(0,id)==blank&&calls==1);
 }
 empty_translation=0;reset_calls();assert(ub_hook_native_lookup(2,0x12b5)==translation&&calls==1);
 empty_translation=1;empty_english=1;reset_calls();assert(ub_hook_native_lookup(2,0x12b5)==blank&&calls==2);
 reset_calls();assert(ub_hook_native_lookup(16,0x12b5)==blank&&calls==1);
 empty_english=0;null_english=1;reset_calls();assert(ub_hook_native_lookup(2,0x12b5)==blank&&calls==2);
 null_english=0;null_translation=1;reset_calls();assert(!ub_hook_native_lookup(2,0x12b5)&&calls==1);
 ub_original_native_lookup=0;reset_calls();assert(!ub_hook_native_lookup(2,0x12b5)&&!calls);
 puts("native_text_test passed: shared direct/explicit lookup; narrow empty fallback; existing/null/outside text preserved");
 return 0;
}
