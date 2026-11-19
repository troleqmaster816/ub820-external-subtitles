#ifndef UB_STORAGE_H
#define UB_STORAGE_H
/* The USB stick is not mounted in Linux; the player reaches it through
 * libufs. The helper carries a small tool (tools/ufs) that copies one file
 * between the stick and /tmp the same way, see core/storage.c. Payloads
 * include this header for the request/status files only. */
#define UB_STICK_DIR "/mnt/usb/sub"
#define UB_STICK_LOG UB_STICK_DIR "/ub820_submenu.log"
#define UB_STICK_PREV UB_STICK_DIR "/ub820_submenu.prev.log" /* save=always: the run before */
/* "save-log <seq>\n", written atomically by the menu; the log module answers
 * in the status file with "<seq> <code> <text>\n" (code 0 = saved). */
#define UB_STORAGE_REQUEST "/tmp/ub820_storage.request"
#define UB_STORAGE_STATUS "/tmp/ub820_storage.status"

/* Results above the tool's own exit codes 2..9 (tools/ufs/ufs.c). Never
 * negative, so a caller can pass any of them on as "applied, not saved".
 * 10 and 11 are the log module's own status codes. */
#define UB_STICK_ABSENT 12  /* read: the directory listing shows no such file */
#define UB_STICK_NO_TOOL 13 /* the tool could not be started or ended abnormally */
#define UB_STICK_TIMEOUT 14 /* deadline passed; outcome unknown, tool retained */
#define UB_STICK_BUSY 15    /* an earlier tool has not ended yet */
#define UB_STICK_UNREAD 16  /* config: the stick's menu file is unreadable, kept in RAM */

#ifndef UB_PAYLOAD
/* 0 copied; otherwise a positive code for ub_stick_error: the tool's exit code
 * or one of UB_STICK_*. Code 4 is any failure to open the source, never proof
 * that it is absent; only UB_STICK_ABSENT is. Only the file itself is read:
 * for a file the helper writes, use ub_stick_load. */
int ub_stick_read(const char *stick_path,const char *tmp_path);
/* The same, without the tool's own message (for files that may be absent). */
int ub_stick_read_quiet(const char *stick_path,const char *tmp_path);
/* Read of a file written with ub_stick_write: when its replacement was
 * interrupted, the previous complete copy is read. quiet as above. */
int ub_stick_load(const char *stick_path,const char *tmp_path,int quiet);
/* Replaces stick_path only with a complete copy; on any failure the earlier
 * file stays readable (see tools/ufs/ufs.c). */
int ub_stick_write(const char *tmp_path,const char *stick_path);
int ub_stick_write_quiet(const char *tmp_path,const char *stick_path);
const char *ub_stick_error(int code);
/* Reaps an overdue tool that had not ended at its deadline; never blocks.
 * Returns how many are still running. Also done by every transfer. */
int ub_stick_reap(void);
#endif
#endif
