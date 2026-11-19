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

/* Publish the complete value by rename: a reader never sees a truncated or
 * partial file. 0 written; -1 failed and the caller retries this write alone. */
static int font_size_set(void) {
 char value[16];int n=snprintf(value,sizeof value,"%d\n",disc_style.font_percent),rc=-1;
 int fd=open(UB_DISC_STYLE_FONT_FILE ".tmp",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0666);
 if(fd<0)return -1;
 if(write(fd,value,(size_t)n)==n)rc=0;
 if(close(fd))rc=-1;
 if(!rc&&rename(UB_DISC_STYLE_FONT_FILE ".tmp",UB_DISC_STYLE_FONT_FILE))rc=-1;
 if(rc)unlink(UB_DISC_STYLE_FONT_FILE ".tmp");
 return rc;
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
 struct ub_code_patch patches[6];
 const unsigned char *want=disc_style.bottom_align?disc_rows_aligned:disc_rows_stock;
 if(style_patch_specs(pid,patches))return -3; /* No transaction: library preflight. */
 patches[5]=(struct ub_code_patch){UB_DISC_ROWS_ADDR,sizeof disc_rows_stock,
  disc_rows_stock,disc_rows_aligned,want};
 return ub_patch_code(pid,patches,6);
}
/* One locked write with the configured position. Never waits for the lock:
 * 0 written, 1 busy (retry on a later tick), -1 failed. */
static int playdat_set_on(void) {
 return playdat_write_on(LOCK_EX|LOCK_NB,disc_style.position);
}
#endif
