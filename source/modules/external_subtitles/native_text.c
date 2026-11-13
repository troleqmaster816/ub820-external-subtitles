/* Narrow fallback at the native lookup shared by gettext_ppro2 and
 * dgettext_ppro2. Native popup providers call gettext directly, so CTK's
 * optional callback alone cannot supply their missing translations. */
typedef const char *(*ub_native_lookup)(unsigned int,unsigned int);
ub_native_lookup ub_original_native_lookup;

const char *ub_hook_native_lookup(unsigned int column,unsigned int text_id){
 const char *text;
 if(!ub_original_native_lookup)return 0;
 text=ub_original_native_lookup(column,text_id);
 if(column!=16u && text_id>=0x12b5u && text_id<=0x12c0u && text && !*text){
  const char *english=ub_original_native_lookup(16u,text_id);
  if(english && *english)return english;
 }
 return text;
}
