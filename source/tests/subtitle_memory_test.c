/* Helper-side subtitle memory without I/O: play_dat observation (arming,
 * stock resets A/B/C, manual zero values, stability, ranges), the memory
 * file (parse, format, merge, untrusted lines), the folder of a chosen file,
 * save scheduling and the RAM record validators. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../modules/external_subtitles/subtitle_memory.h"

static unsigned run(struct prefs_obs *o,const unsigned char s[8],int loaded,unsigned times){
 unsigned mask=0;for(unsigned i=0;i<times;i++)mask|=prefs_sample(o,s,loaded);return mask;
}
static void prefs_tests(unsigned *cases){
 struct prefs_obs o;
 const unsigned char start[8]={(unsigned char)-8,0,0,0,0,1,7,9};
 unsigned char s[8];
 /* not armed: nothing is ever a choice */
 prefs_disarm(&o);memcpy(s,start,8);s[0]=(unsigned char)-16;assert(!run(&o,s,1,5));(*cases)++;
 /* a manual change counts after three stable readings, once */
 prefs_arm(&o,start);memcpy(s,start,8);s[0]=(unsigned char)-16;
 assert(!prefs_sample(&o,s,1)&&!prefs_sample(&o,s,1));
 assert(prefs_sample(&o,s,1)==1u<<PREF_POSITION&&(signed char)o.base[0]==-16);
 assert(!run(&o,s,1,5));(*cases)++;
 /* manual position 0 and sync back to 0 are choices, not resets */
 prefs_arm(&o,start);memcpy(s,start,8);s[0]=(unsigned char)-1;assert(run(&o,s,1,3)==1u<<PREF_POSITION);
 s[0]=0;assert(run(&o,s,1,3)==1u<<PREF_POSITION&&o.base[0]==0);
 s[3]=1;assert(run(&o,s,1,3)==1u<<PREF_SYNC);s[3]=0;assert(run(&o,s,1,3)==1u<<PREF_SYNC);(*cases)++;
 /* reset A/B (dispsrv: all six bytes 0, byte5 included) is never saved */
 prefs_arm(&o,start);memcpy(s,start,8);s[0]=(unsigned char)-16;prefs_sample(&o,s,1);
 memset(s,0,6);assert(!run(&o,s,1,5));assert(!memcmp(o.base,s,6));(*cases)++;
 /* reset C (apl_bdv discEjectAck: byte0 and byte1) from a far position */
 prefs_arm(&o,start);memcpy(s,start,8);s[0]=0;s[1]=0;assert(!run(&o,s,1,5)&&o.base[0]==0);
 {unsigned char k[8];memcpy(k,start,8);k[0]=(unsigned char)-1;k[1]=(unsigned char)-4;prefs_arm(&o,k);
  memcpy(s,k,8);s[0]=0;s[1]=0;assert(!run(&o,s,1,5));}(*cases)++;
 /* half-written stock series (one field at a time) never accumulates into a save */
 prefs_arm(&o,start);memcpy(s,start,8);s[2]=2;prefs_sample(&o,s,1);s[5]=0;assert(!run(&o,s,1,4));(*cases)++;
 /* engine not loaded / playback ending: pending candidates are dropped */
 prefs_arm(&o,start);memcpy(s,start,8);s[2]=2;prefs_sample(&o,s,1);prefs_sample(&o,s,1);
 assert(!prefs_sample(&o,s,0));assert(!prefs_sample(&o,s,1)&&!prefs_sample(&o,s,1)&&prefs_sample(&o,s,1)==1u<<PREF_SIZE);(*cases)++;
 /* a value that keeps changing is not taken until it settles */
 prefs_arm(&o,start);memcpy(s,start,8);
 for(int v=-9;v>=-14;v--){s[0]=(unsigned char)(signed char)v;assert(!prefs_sample(&o,s,1));}
 assert(run(&o,s,1,2)==1u<<PREF_POSITION&&(signed char)o.base[0]==-14);(*cases)++;
 /* out-of-range values are followed as the reference but not reported */
 prefs_arm(&o,start);memcpy(s,start,8);s[2]=7;assert(!run(&o,s,1,3)&&o.base[2]==7);
 s[0]=(unsigned char)40;assert(!run(&o,s,1,3));
 s[0]=(unsigned char)-32;assert(run(&o,s,1,3)==1u<<PREF_POSITION);
 s[0]=32;assert(run(&o,s,1,3)==1u<<PREF_POSITION);(*cases)++;
 /* two fields at once (position then size within one second) both count */
 prefs_arm(&o,start);memcpy(s,start,8);s[0]=(unsigned char)-10;s[2]=1;assert(run(&o,s,1,3)==((1u<<PREF_POSITION)|(1u<<PREF_SIZE)));(*cases)++;
 /* luminance (byte1), byte5 and bytes 6/7 are never reported */
 prefs_arm(&o,start);memcpy(s,start,8);s[1]=(unsigned char)-4;s[6]=1;s[7]=2;assert(!run(&o,s,1,4));(*cases)++;
}
static void memory_tests(unsigned *cases){
 static struct sub_memory m,file;static char out[96*1024];size_t n;char dir[256];
 const char *text=
  "# UB820 Helper subtitle memory v1\r\n"
  "last_dir=/mnt/usb/Filmy i seriale\n"
  "disc=0123456789abcdef0123456789abcdef 1 -2 /mnt/usb/Filmy i seriale/A.srt\n"
  "disc=0123456789abcdef0123456789abcdef 0 0 /mnt/usb/dup.srt\n"          /* duplicate id: newest wins */
  "disc=0123456789ABCDEF0123456789abcdef 0 0 /mnt/usb/x.srt\n"            /* upper-case id */
  "disc=fedcba9876543210fedcba9876543210 2 0 /mnt/usb/x.srt\n"            /* charcode */
  "disc=fedcba9876543210fedcba9876543210 0 31 /mnt/usb/x.srt\n"           /* sync */
  "disc=fedcba9876543210fedcba9876543210 0 0 /mnt/usb/x.txt\n"            /* not .srt */
  "disc=fedcba9876543210fedcba9876543210 0 0 /mnt/usb/../etc/x.srt\n"     /* dot-dot */
  "disc=fedcba9876543210fedcba9876543210 0 0 /tmp/x.srt\n"                /* outside /mnt/usb */
  "last_dir=/mnt/usb/\n"                                                  /* trailing slash */
  "bogus=1\n"
  "\n";
 sub_memory_parse(&m,text);
 assert(!strcmp(m.last_dir,"/mnt/usb/Filmy i seriale")&&m.count==1&&m.rejected==8);
 assert(m.disc[0].charcode==1&&m.disc[0].sync==-2&&!strcmp(m.disc[0].path,"/mnt/usb/Filmy i seriale/A.srt"));(*cases)++;
 /* an over-long line is rejected as a whole, the rest still parses */
 {static char big[2048];memset(big,'a',sizeof big);memcpy(big,"last_dir=/mnt/usb/",18);strcpy(big+1000,"\nlast_dir=/mnt/usb/ok\n");
  sub_memory_parse(&m,big);assert(!strcmp(m.last_dir,"/mnt/usb/ok")&&m.rejected==1);}(*cases)++;
 /* format and parse are inverse; the limit is kept */
 memset(&m,0,sizeof m);strcpy(m.last_dir,"/mnt/usb/a");
 for(unsigned i=0;i<UB_SUB_MEMORY_MAX;i++){snprintf(m.disc[i].id,sizeof m.disc[i].id,"%032x",i);m.disc[i].charcode=(int)(i&1);m.disc[i].sync=(int)(i%61)-30;snprintf(m.disc[i].path,sizeof m.disc[i].path,"/mnt/usb/f %u.srt",i);}
 m.count=UB_SUB_MEMORY_MAX;n=sub_memory_format(&m,out,sizeof out);assert(n&&n<sizeof out);
 sub_memory_parse(&file,out);assert(file.count==UB_SUB_MEMORY_MAX&&!file.rejected&&!strcmp(file.last_dir,"/mnt/usb/a"));
 assert(!memcmp(&file.disc[199],&m.disc[199],sizeof m.disc[199]));
 assert(!sub_memory_format(&m,out,64));(*cases)++;
 /* merge: this run's values win, the file's other entries join */
 memset(&m,0,sizeof m);strcpy(m.last_dir,"/mnt/usb/run");m.count=1;strcpy(m.disc[0].id,"0123456789abcdef0123456789abcdef");strcpy(m.disc[0].path,"/mnt/usb/run.srt");
 memset(&file,0,sizeof file);strcpy(file.last_dir,"/mnt/usb/file");file.count=2;
 strcpy(file.disc[0].id,"0123456789abcdef0123456789abcdef");strcpy(file.disc[0].path,"/mnt/usb/old.srt");
 strcpy(file.disc[1].id,"fedcba9876543210fedcba9876543210");strcpy(file.disc[1].path,"/mnt/usb/other.srt");
 sub_memory_merge(&m,&file);assert(!strcmp(m.last_dir,"/mnt/usb/run")&&m.count==2&&!strcmp(m.disc[0].path,"/mnt/usb/run.srt")&&!strcmp(m.disc[1].path,"/mnt/usb/other.srt"));
 memset(&m,0,sizeof m);sub_memory_merge(&m,&file);assert(!strcmp(m.last_dir,"/mnt/usb/file"));(*cases)++;
 /* folder of a chosen file */
 assert(!sub_dirname("/mnt/usb/Filmy/a.srt",dir,sizeof dir)&&!strcmp(dir,"/mnt/usb/Filmy"));
 assert(!sub_dirname("/mnt/usb/a.srt",dir,sizeof dir)&&!strcmp(dir,"/mnt/usb"));
 assert(sub_dirname("/mnt/a.srt",dir,sizeof dir)&&sub_dirname("a.srt",dir,sizeof dir));(*cases)++;
}
static void sched_tests(unsigned *cases){
 struct save_sched s;memset(&s,0,sizeof s);
 assert(!sched_due(&s,1000,1));
 sched_changed(&s,1000);assert(!sched_due(&s,1000+SAVE_DEBOUNCE_MS-1,0)&&sched_due(&s,1000+SAVE_DEBOUNCE_MS,0)&&sched_due(&s,1001,1));
 sched_result(&s,10000,1);assert(!s.dirty);sched_changed(&s,10001);assert(!sched_due(&s,10000+SAVE_MIN_GAP_MS-1,1)&&sched_due(&s,10000+SAVE_MIN_GAP_MS,1));(*cases)++;
 /* failures: 15 s, then 60 s, then held until a new change or a forced moment */
 memset(&s,0,sizeof s);sched_changed(&s,0);
 sched_result(&s,100000,0);assert(!sched_due(&s,100000+SAVE_MIN_GAP_MS-1,0)&&sched_due(&s,100000+SAVE_MIN_GAP_MS,0));
 sched_result(&s,200000,0);assert(!sched_due(&s,200000+SAVE_RETRY_LONG_MS-1,1)&&sched_due(&s,200000+SAVE_RETRY_LONG_MS,0));
 sched_result(&s,300000,0);assert(s.held&&!sched_due(&s,900000,0)&&sched_due(&s,900000,1));
 sched_changed(&s,900000);assert(!s.held&&sched_due(&s,900000+SAVE_DEBOUNCE_MS,0));(*cases)++;
}
static void record_tests(unsigned *cases){
 struct ub_sub_hint h;struct ub_sub_selection sel;
 memset(&h,0,sizeof h);h.magic=UB_SUB_HINT_MAGIC;h.version=UB_SUB_RECORD_VERSION;h.pid=7;h.birth_lo=9;h.flags=UB_SUB_HINT_START_DIR;strcpy(h.start_dir,"/mnt/usb/a");
 assert(ub_sub_hint_valid(&h,7,9)&&!ub_sub_hint_valid(&h,7,10)&&!ub_sub_hint_valid(&h,8,9)&&!ub_sub_hint_valid(&h,0,9));
 h.sync=31;assert(!ub_sub_hint_valid(&h,7,9));h.sync=-30;h.charcode=2;assert(!ub_sub_hint_valid(&h,7,9));h.charcode=1;
 h.flags|=8;assert(!ub_sub_hint_valid(&h,7,9));h.flags=UB_SUB_HINT_START_DIR|UB_SUB_HINT_AUTOLOAD;assert(!ub_sub_hint_valid(&h,7,9)); /* empty autoload */
 strcpy(h.autoload,"/mnt/usb/a/x.srt");assert(ub_sub_hint_valid(&h,7,9));
 memset(h.start_dir,'a',sizeof h.start_dir);assert(!ub_sub_hint_valid(&h,7,9));(*cases)++;
 memset(&sel,0,sizeof sel);sel.magic=UB_SUB_SELECTION_MAGIC;sel.version=UB_SUB_RECORD_VERSION;sel.pid=7;sel.birth_lo=9;sel.seq=1;sel.kind=UB_SUB_KIND_CHOSEN;strcpy(sel.path,"/mnt/usb/a.srt");
 assert(ub_sub_selection_valid(&sel,7,9));sel.kind=4;assert(!ub_sub_selection_valid(&sel,7,9));sel.kind=1;sel.seq=0;assert(!ub_sub_selection_valid(&sel,7,9));
 sel.seq=1;strcpy(sel.path,"/mnt/usb//a.srt");assert(!ub_sub_selection_valid(&sel,7,9));(*cases)++;
 assert(ub_sub_path_ok("/mnt/usb",9)&&!ub_sub_path_ok("/mnt/usbx",10)&&!ub_sub_path_ok("/mnt/usb/a\\b",13)&&!ub_sub_path_ok("/mnt/usb/./a",13));(*cases)++;
}
/* Disc memory: LRU entries, what a selection teaches, sync/charcode follow. */
static void disc_tests(unsigned *cases){
 static struct sub_memory m,back;static char text[96*1024];
 const char *A="0123456789abcdef0123456789abcdef",*B="fedcba9876543210fedcba9876543210";
 unsigned char s[8]={(unsigned char)-8,0,0,(unsigned char)-4,1,1,0,0};
 memset(&m,0,sizeof m);
 /* put: new entry first; refused input leaves the memory alone */
 assert(sub_memory_put(&m,A,"/mnt/usb/Filmy/a b.srt",1,-4)==1&&m.count==1);
 assert(sub_memory_put(&m,A,"/mnt/usb/Filmy/a b.srt",1,-4)==0); /* unchanged */
 assert(sub_memory_put(&m,B,"/mnt/usb/b.srt",0,0)==1&&m.count==2&&!strcmp(m.disc[0].id,B));
 assert(sub_memory_put(&m,"0123456789ABCDEF0123456789abcdef","/mnt/usb/c.srt",0,0)==-1); /* upper case id */
 assert(sub_memory_put(&m,A,"/mnt/usb/c.txt",0,0)==-1&&sub_memory_put(&m,A,"/mnt/usb/../c.srt",0,0)==-1);
 assert(sub_memory_put(&m,A,"/mnt/usb/c.srt",2,0)==-1&&sub_memory_put(&m,A,"/mnt/usb/c.srt",0,31)==-1&&m.count==2);(*cases)++;
 /* front: an existing entry moves first without changing its content */
 assert(sub_memory_front(&m,1)&&!strcmp(m.disc[0].id,A)&&!strcmp(m.disc[1].id,B)&&m.disc[0].sync==-4);
 assert(!sub_memory_front(&m,0)&&!sub_memory_front(&m,5));
 assert(sub_memory_find(&m,B)==1&&sub_memory_find(&m,"00000000000000000000000000000000")<0);(*cases)++;
 /* LRU: the 201st disc drops the oldest */
 memset(&m,0,sizeof m);
 for(unsigned i=0;i<=UB_SUB_MEMORY_MAX;i++){char id[33],path[64];snprintf(id,sizeof id,"%032x",i);snprintf(path,sizeof path,"/mnt/usb/%u.srt",i);assert(sub_memory_put(&m,id,path,0,0)==1);}
 assert(m.count==UB_SUB_MEMORY_MAX&&sub_memory_find(&m,"00000000000000000000000000000000")<0);
 assert(!strcmp(m.disc[0].id,"000000000000000000000000000000c8")&&!strcmp(m.disc[UB_SUB_MEMORY_MAX-1].id,"00000000000000000000000000000001"));
 /* and survives the file round trip in that order */
 {size_t n=sub_memory_format(&m,text,sizeof text);assert(n);sub_memory_parse(&back,text);
  assert(back.count==m.count&&!back.rejected&&!memcmp(back.disc,m.disc,sizeof m.disc[0]*m.count));}(*cases)++;
 /* a chosen file is remembered with the current sync/charcode */
 memset(&m,0,sizeof m);
 assert(disc_learn_selection(&m,A,UB_SUB_KIND_CHOSEN,"/mnt/usb/x.srt",s)&&m.count==1&&m.disc[0].sync==-4&&m.disc[0].charcode==1);
 assert(!disc_learn_selection(&m,A,UB_SUB_KIND_CHOSEN,"/mnt/usb/x.srt",s)); /* same again */
 /* another file for the same disc replaces it and takes the current values */
 s[3]=0;s[4]=0;assert(disc_learn_selection(&m,A,UB_SUB_KIND_CHOSEN,"/mnt/usb/y.srt",s)&&m.count==1&&!strcmp(m.disc[0].path,"/mnt/usb/y.srt")&&!m.disc[0].sync&&!m.disc[0].charcode);
 /* out-of-range play_dat bytes are stored as the stock values */
 s[3]=(unsigned char)99;s[4]=7;assert(disc_learn_selection(&m,B,UB_SUB_KIND_CHOSEN,"/mnt/usb/z.srt",s)&&m.disc[0].sync==0&&m.disc[0].charcode==0);(*cases)++;
 /* sub.srt never creates an entry; no disc id, nothing */
 assert(!disc_learn_selection(&m,"00000000000000000000000000000001",UB_SUB_KIND_DEFAULT,UB_DEFAULT_SRT,s)&&m.count==2);
 assert(!disc_learn_selection(&m,"",UB_SUB_KIND_CHOSEN,"/mnt/usb/x.srt",s)&&m.count==2);
 /* the remembered file only moves its entry first (values unchanged) */
 assert(!strcmp(m.disc[0].id,B));
 s[3]=5;assert(disc_learn_selection(&m,A,UB_SUB_KIND_REMEMBERED,"/mnt/usb/y.srt",s)&&!strcmp(m.disc[0].id,A)&&m.disc[0].sync==0);
 assert(!disc_learn_selection(&m,A,UB_SUB_KIND_REMEMBERED,"/mnt/usb/y.srt",s));(*cases)++;
 /* sync/charcode changes follow the entry only for kinds 2 and 3 */
 {unsigned char base[8]={0,0,0,(unsigned char)-7,1,1,0,0};unsigned mask=1u<<PREF_SYNC|1u<<PREF_CODE;
  assert(!disc_learn_prefs(&m,A,UB_SUB_KIND_DEFAULT,mask,base)&&m.disc[0].sync==0);
  assert(!disc_learn_prefs(&m,"",UB_SUB_KIND_CHOSEN,mask,base));
  assert(!disc_learn_prefs(&m,A,UB_SUB_KIND_CHOSEN,1u<<PREF_POSITION,base));
  assert(disc_learn_prefs(&m,A,UB_SUB_KIND_REMEMBERED,1u<<PREF_SYNC,base)&&m.disc[0].sync==-7&&m.disc[0].charcode==0);
  assert(disc_learn_prefs(&m,A,UB_SUB_KIND_CHOSEN,mask,base)&&m.disc[0].charcode==1);
  assert(!disc_learn_prefs(&m,A,UB_SUB_KIND_CHOSEN,mask,base)); /* same values */
  assert(!disc_learn_prefs(&m,"00000000000000000000000000000009",UB_SUB_KIND_CHOSEN,mask,base));}(*cases)++;
 /* a new selection's play_dat is the reference: a failed autoload's reset
  * (sync/charcode to 0) is not taken for a viewer's change */
 {struct prefs_obs o;unsigned char st[8]={(unsigned char)-8,0,0,2,1,1,0,0};
  prefs_arm(&o,st);st[3]=0;st[4]=0;prefs_rebase_file(&o,st);
  assert(!run(&o,st,1,5));
  st[3]=3;assert(run(&o,st,1,3)==1u<<PREF_SYNC&&o.base[PREF_SYNC]==3);
  prefs_disarm(&o);prefs_rebase_file(&o,st);assert(!o.armed);}(*cases)++;
}
int main(void){
 unsigned cases=0;
 prefs_tests(&cases);memory_tests(&cases);sched_tests(&cases);record_tests(&cases);disc_tests(&cases);
 printf("subtitle_memory_test passed: %u cases; arming, stock resets A/B/C, manual zero values, stability and ranges; memory file; scheduling; records; disc LRU, learning and sync/charcode follow\n",cases);
 return 0;
}
