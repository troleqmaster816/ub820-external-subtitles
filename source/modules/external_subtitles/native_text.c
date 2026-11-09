/* Natural OPTIONS builder hook: install CTK's supported gettext callback on
 * the UI thread. No constructors, language switch, or replacement mappings. */
#include <stdint.h>

typedef const char *(*ub_gettext_callback)(unsigned int,unsigned int);
extern const char *dgettext_ppro2(unsigned int,unsigned int);
extern void CTK_util_set_gettextfunc(ub_gettext_callback);

/* Bound only after the supervisor pins libctk/libmulang and their bases. */
ub_gettext_callback volatile *ub_gettext_slot;
ub_gettext_callback ub_stock_gettext_callback;
void (*ub_original_native_rows)(void);
static ub_gettext_callback original_gettext;

const char *ub_native_english_gettext(unsigned int language,unsigned int text_id){
 ub_gettext_callback original=__atomic_load_n(&original_gettext,__ATOMIC_ACQUIRE);
 const char *text=(original?original:dgettext_ppro2)(language,text_id);
 if(text_id>=0x12b5u && text_id<=0x12c0u && text && !*text){
  const char *english=dgettext_ppro2(0x20u,text_id);
  if(english && *english)return english;
 }
 return text;
}
void ub_hook_native_rows(void){
 /* Preserve foreign callbacks: only the verified stock callback is ours to
  * wrap. Repeated OPTIONS openings neither nest wrappers nor recurse. */
 if(ub_gettext_slot){
  ub_gettext_callback current=*ub_gettext_slot;
  if(current && (current==dgettext_ppro2 || current==ub_stock_gettext_callback)){
   __atomic_store_n(&original_gettext,current,__ATOMIC_RELAXED);
   __sync_synchronize(); /* Publish the chain before CTK publishes the wrapper. */
   CTK_util_set_gettextfunc(ub_native_english_gettext);
  }
 }
 if(ub_original_native_rows)ub_original_native_rows();
}
