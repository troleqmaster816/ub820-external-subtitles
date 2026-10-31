#ifndef UB_LOG_H
#define UB_LOG_H
/* One RAM log for the helper and every module. UB_ALWAYS lines (errors and
 * essential lifecycle events) are always written; UB_INFO/UB_DEBUG only up to
 * the level the log module allows. Lines are written with one write() each. */
#ifndef UB_LOG_FILE
#define UB_LOG_FILE "/tmp/ub820_submenu.log"
#endif
enum { UB_ALWAYS=0, UB_INFO=1, UB_DEBUG=2 };
/* Created by another process (crash reporter, ssh guard) to have the log
 * module save the log to the stick at once (save=always). */
#define UB_LOG_URGENT "/tmp/ub820_log.urgent"
extern int ub_log_level;
void ub_log_open(void);
void ub_log(const char *module,int level,const char *format,...) __attribute__((format(printf,3,4)));
#endif
