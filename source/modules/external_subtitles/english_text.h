/* English text for native subtitle rows and popups. Runtime hooks their shared
 * native lookup, with exact executable/library/language pins and mapped base.
 * No callback replacement or language switch. The old pure page
 * builder below remains an offline language-data fixture for lang_test only;
 * production code never calls it or ub_overlay_pages(). */
#ifndef UB_ENGLISH_TEXT_H
#define UB_ENGLISH_TEXT_H
#define LANG_ID "external_subtitles"
#define LANG_SUFFIX "/data/lang/apl_lang.dat"
#define LANG_SHA256 "a50b218d480c68cae126457c3a734d928af2acdc81c318e09030972da7cd59df"
#define LANG_PAGE 0x408000u
#define LANG_PAGES 0x2000u
#define LANG_FIRST 0x12b5u
#define LANG_LAST 0x12c0u
#define LANG_ENGLISH 16u

#ifdef UB_LANG_BUILD_ONLY
static unsigned char lang_old[LANG_PAGES],lang_new[LANG_PAGES];
static int lang_state; /* 0 not built, 1 ready, -1 unusable */
static int lang_inside(uint32_t at,uint32_t n){return at>=LANG_PAGE&&at-LANG_PAGE<=LANG_PAGES-n;}
static int lang_build(const char *path){
 char h[65];int fd;uint32_t i,k,rel,entry,english,at;uint16_t off[24];
 if(lang_state)return lang_state;
 lang_state=-1;
 if(ub_hash_file(path,h)||strcmp(h,LANG_SHA256)){ub_log(LANG_ID,UB_ALWAYS,"%s differs from 1.82; English text fallback skipped",path);return -1;}
 fd=open(path,O_RDONLY);if(fd<0)return -1;
 if(pread(fd,lang_old,LANG_PAGES,LANG_PAGE)!=(ssize_t)LANG_PAGES){close(fd);return -1;}
 memcpy(lang_new,lang_old,LANG_PAGES);
 for(i=LANG_FIRST;i<=LANG_LAST;i++){
  if(pread(fd,&rel,4,4*i)!=4){close(fd);return -1;}
  entry=rel+4*i;if(!lang_inside(entry,sizeof off)){close(fd);return -1;}
  memcpy(off,lang_old+entry-LANG_PAGE,sizeof off);
  english=entry+2*LANG_ENGLISH+off[LANG_ENGLISH];
  if(!lang_inside(english,1)||!lang_old[english-LANG_PAGE])continue;
  for(k=0;k<LANG_ENGLISH;k++){
   uint16_t value;at=entry+2*k+off[k];
   if(!lang_inside(at,1)||lang_old[at-LANG_PAGE])continue;
   value=(uint16_t)(english-(entry+2*k));memcpy(lang_new+entry-LANG_PAGE+2*k,&value,2);
  }
 }
 close(fd);
 lang_state=memcmp(lang_old,lang_new,LANG_PAGES)?1:-1;return lang_state;
}
#endif

#ifndef UB_LANG_BUILD_ONLY
extern const struct ub_payload ub_payload_external_subtitles_native_text;
/* Installation stays separate from ui/menu so english_text also works with
 * [ui] disabled. Core's normal payload installer cannot preflight these data
 * dependencies, so this guarded part installs its payload after validating
 * the selected executable's matching library and language-data cohort.
 * 0 mapping not yet present; 1 verified; -1 incompatible, make no changes. */
static int lang_dependency(int pid,const char *suffix,uint32_t expected_base,const char *expected_sha){
 char path[600],digest[65];uint32_t base=ub_map_base(pid,suffix,path,sizeof path);
 if(!base)return 0;
 if((expected_base && base!=expected_base) || ub_hash_file(path,digest) ||
    strcmp(digest,expected_sha)){
  ub_log(LANG_ID,UB_ALWAYS,"%s mapping/file does not match the selected firmware cohort; native English text skipped",path);
  return -1;
 }
 return 1;
}
#define LANG_ATTEMPTS 12
#define LANG_RETRY_SECONDS 2
static void english_text_step(struct ub_part *p,struct ub_target *t){
 int rc;
 if(t->state[0])return;
 if(t->state[3]){t->state[3]--;return;}
 if(!ub_settled("apl_dispsrv"))return;
 if(++t->state[1]>120){t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"native text dependencies not mapped in PID%d; skipped",t->pid);return;}
 if(!t->state[4]){
  char path[64],digest[65];const struct ub_payload_variant *variant;
  snprintf(path,sizeof path,"/proc/%d/exe",t->pid);
  if(ub_hash_file(path,digest))return;
  variant=ub_payload_variant(&ub_payload_external_subtitles_native_text,digest);
  if(!variant||!variant->sha256||strcmp(variant->sha256,digest)||!variant->firmware||
     (strcmp(variant->firmware,"1.82")&&strcmp(variant->firmware,"1.85"))){
   t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"native text has no exact firmware cohort for PID%d; skipped",t->pid);return;
  }
  t->state[4]=!strcmp(variant->firmware,"1.85")?185:182;
 }
 rc=lang_dependency(t->pid,"/libmulang.so",0x29450000u,
  t->state[4]==185?"9856df99ec704146ecdd6019c8166b15c8a6b9550cf67624f429a56a363b1bb4":
  "dddd5af0fda3f8accde5a4006fe0160f9aa2292a2ae8fd973849af8071a8d8b5");
 if(rc<1){if(rc<0)t->state[0]=-1;return;}
 rc=lang_dependency(t->pid,LANG_SUFFIX,0,
  t->state[4]==185?"f83f9367d53cfea7dda8cd1d0536c9fb1699466138c20499d0e3be06c102e44e":LANG_SHA256);
 if(rc<1){if(rc<0)t->state[0]=-1;return;}
 rc=ub_install_payload(t->pid,&ub_payload_external_subtitles_native_text);
 if(!rc){t->installed=1;t->state[0]=1;ub_changed(p);ub_log(LANG_ID,UB_INFO,"Native English lookup fallback installed in PID%d (firmware %s); rows and popups use it on lookup",t->pid,t->state[4]==185?"1.85":"1.82");}
 else if(rc==-2){t->failed=1;t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"Native English text installation quarantined PID%d",t->pid);}
 else if(++t->state[2]>=LANG_ATTEMPTS){t->failed=1;t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"Native English text installation stopped at %s after %d attempts",ub_install_stage(),t->state[2]);}
 else{t->state[3]=LANG_RETRY_SECONDS;ub_log(LANG_ID,UB_INFO,"Native English text attempt %d stalled at %s; retrying",t->state[2],ub_install_stage());}
}

#endif
#endif
