/* English text for the native subtitle rows in European menu languages
 * (option english_text). Included by module.c after ub.h.
 *
 * The rows' text IDs 0x12b5..0x12c0 (E559_o..E570_o) have strings only in
 * libmulang's regional columns 16..22; columns 0..15 (all European menu
 * languages) are empty. Each entry is 24 u16 offsets, slot-relative, followed
 * by its strings; column 16 is English. libmulang maps the file read-only and
 * shared, so the two pages holding these entries are swapped for a private
 * copy in which each empty column points at the entry's own English string.
 * No code, GOT or file is changed; every other text ID keeps its bytes. */
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

/* While the player UI is still starting, its threads may not all pause at
 * once; try again every 5 s, like the payload install, for about a minute.
 * state: [0] 1 done / -1 given up, [1] seconds waiting for the mapping,
 * [2] failed attempts, [3] seconds until the next attempt */
#define LANG_ATTEMPTS 12
#define LANG_RETRY_SECONDS 2
static void english_text_step(struct ub_part *p,struct ub_target *t){
 char path[600];uint32_t base;int rc;
 if(t->state[0])return;
 if(t->state[3]){t->state[3]--;return;}
 if(!ub_settled("apl_dispsrv"))return; /* one ptrace session at a time */
 if(++t->state[1]>120){t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"language data not mapped in PID%d; skipped",t->pid);return;}
 base=ub_map_base(t->pid,LANG_SUFFIX,path,sizeof path);
 if(!base)return;
 if(lang_build(path)!=1){t->state[0]=-1;return;}
 rc=ub_overlay_pages(t->pid,base+LANG_PAGE,lang_old,lang_new,LANG_PAGES);
 if(rc==0){t->state[0]=1;ub_changed(p);ub_log(LANG_ID,UB_INFO,"English text for native subtitle rows active in PID%d",t->pid);}
 else if(rc==1){t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"language pages in PID%d differ from 1.82; skipped",t->pid);}
 else if(++t->state[2]>=LANG_ATTEMPTS){t->state[0]=-1;ub_log(LANG_ID,UB_ALWAYS,"English text could not be applied in PID%d: %s (%d attempts)",t->pid,ub_overlay_error(rc),t->state[2]);}
 else{t->state[3]=LANG_RETRY_SECONDS;ub_log(LANG_ID,UB_INFO,"English text attempt %d in PID%d: %s; retrying",t->state[2],t->pid,ub_overlay_error(rc));}
}

#endif
