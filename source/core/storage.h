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

#ifndef UB_PAYLOAD
/* 0 copied; 1..9 the tool's exit code (see tools/ufs/ufs.c); -1 the tool could
 * not be started; -2 it did not finish in time. */
int ub_stick_read(const char *stick_path,const char *tmp_path);
/* The same, without the tool's own message (for files that may be absent). */
int ub_stick_read_quiet(const char *stick_path,const char *tmp_path);
int ub_stick_write(const char *tmp_path,const char *stick_path);
const char *ub_stick_error(int code);
#endif
#endif
