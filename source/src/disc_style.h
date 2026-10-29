/* Optional calibration and stable one/two-line bottom baseline for stock 1.82.
 * Included after engage_core.c. No firmware-file writes.
 */
#ifndef UB_DISC_STYLE_H
#define UB_DISC_STYLE_H
#include <ctype.h>
#ifndef UB_DISC_STYLE_CONFIG
#define UB_DISC_STYLE_CONFIG "/mnt/usb/sub/disc_style.cfg"
#endif
#ifndef UB_DISC_STYLE_FONT_FILE
#define UB_DISC_STYLE_FONT_FILE "/tmp/ti_font_size"
#endif
struct ub_disc_style { int font_percent, position, bottom_align; };
static struct ub_disc_style disc_style = {122,-8,1};

static void disc_style_parse(FILE *file) {
 char line[128]; unsigned count=0;
 while(count++<32 && fgets(line,sizeof line,file)) {
  char *p=line,*end,*key,*eq;long value;
  while(isspace((unsigned char)*p))p++;
  if(!*p || *p=='#')continue;
  eq=strchr(p,'=');if(!eq)continue;
  *eq=0;key=p;end=eq;
  while(end>key && isspace((unsigned char)end[-1]))*--end=0;
  p=eq+1;errno=0;value=strtol(p,&end,10);
  if(end==p || errno)continue;
  while(isspace((unsigned char)*end))end++;
  if(*end && *end!='#')continue;
  if(!strcmp(key,"font_percent") && value>=90 && value<=150)disc_style.font_percent=(int)value;
  else if(!strcmp(key,"position") && value>=-10 && value<=10)disc_style.position=(int)value;
  else if(!strcmp(key,"bottom_align") && value>=0 && value<=1)disc_style.bottom_align=(int)value;
 }
}
static void disc_style_load(void) {
 FILE *f=fopen(UB_DISC_STYLE_CONFIG,"r");
 if(f){disc_style_parse(f);fclose(f);}
 fprintf(stderr,"Disc style: font=%d%% position=%d bottom_align=%d\n",
         disc_style.font_percent,disc_style.position,disc_style.bottom_align);
}
static void font_size_set(void) {
 char value[16];int n=snprintf(value,sizeof value,"%d\n",disc_style.font_percent);
 int fd=open(UB_DISC_STYLE_FONT_FILE,O_WRONLY|O_CREAT|O_TRUNC,0666);
 if(fd>=0){if(write(fd,value,(size_t)n)!=n)fprintf(stderr,"Font calibration write failed\n");close(fd);}
 else fprintf(stderr,"Font calibration file open failed\n");
}

/* In esl_tst_UpdateDisplayText, r3 holds the counted number of lines.
 * Stock chooses offset 1 for both one and two lines: {row1}, {row1,row2}.
 * Choose 2-count for <=2: {row1}, {row0,row1}. All three/four-line native
 * behavior is retained. Re-encode the last ADD as a 16-bit instruction so
 * the new arithmetic fits the same 16 bytes, with no displaced branches.
 */
#define UB_DISC_ROWS_ADDR 0x1f98eUL
static const unsigned char disc_rows_stock[16]={
 0x02,0x2b,0x05,0xd8,0x01,0x23,0x07,0xf5,
 0xa3,0x52,0x02,0xf1,0x0f,0x02,0x13,0x70};
static const unsigned char disc_rows_aligned[16]={
 0x02,0x2b,0x05,0xd8,0xc3,0xf1,0x02,0x03,
 0x07,0xf5,0xa3,0x52,0x0f,0x32,0x13,0x70};
static int style_patch(int pid) {
 unsigned char cur[16];
 const unsigned char *want=disc_style.bottom_align?disc_rows_aligned:disc_rows_stock;
 if(legacy_style_patch(pid))return -1;
 if(mem_read_buf(pid,UB_DISC_ROWS_ADDR,cur,sizeof cur))return -1;
 if(memcmp(cur,disc_rows_stock,sizeof cur) && memcmp(cur,disc_rows_aligned,sizeof cur)) {
  fprintf(stderr,"Disc layout bytes differ from exact 1.82; layout patch skipped\n");return -1;
 }
 return style_site(pid,UB_DISC_ROWS_ADDR,want,sizeof cur);
}
static int playdat_set_on(void) {
 int lf,fd,rc=-1;unsigned char value=(unsigned char)disc_style.position;
 if(legacy_playdat_set_on())return -1;
 if(disc_style.position==-8)return 0;
 lf=open(PLAY_LOCK,O_CREAT|O_RDWR,0666);if(lf<0)return -1;
 if(flock(lf,LOCK_EX)){close(lf);return -1;}
 fd=open(PLAY_DAT,O_WRONLY);
 if(fd>=0){if(pwrite(fd,&value,1,0)==1)rc=0;close(fd);}
 flock(lf,LOCK_UN);close(lf);return rc;
}
#endif
