/* Optional calibration and stable one/two-line bottom baseline for stock 1.82.
 * Included after engage_core.c and ub.h. No firmware-file writes.
 */
#ifndef UB_DISC_STYLE_H
#define UB_DISC_STYLE_H
#ifndef UB_DISC_STYLE_FONT_FILE
#define UB_DISC_STYLE_FONT_FILE "/tmp/ti_font_size"
#endif
/* Values come from the module options (sub/modules.cfg [external_subtitles],
 * or the older sub/disc_style.cfg keys). */
struct ub_disc_style { int font_percent, position, bottom_align; };
static struct ub_disc_style disc_style = {122,-8,1};

static void font_size_set(void) {
 char value[16];int n=snprintf(value,sizeof value,"%d\n",disc_style.font_percent);
 int fd=open(UB_DISC_STYLE_FONT_FILE,O_WRONLY|O_CREAT|O_TRUNC,0666);
 if(fd>=0){if(write(fd,value,(size_t)n)!=n)ub_log("external_subtitles",UB_ALWAYS,"Font calibration write failed");close(fd);}
 else ub_log("external_subtitles",UB_ALWAYS,"Font calibration file open failed");
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
  ub_log("external_subtitles",UB_ALWAYS,"Disc layout bytes differ from exact 1.82; layout patch skipped");return -1;
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
