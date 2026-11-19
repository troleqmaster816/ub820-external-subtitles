#ifndef UB_SYS_H
#define UB_SYS_H
/* The player runs Linux 4.4 (arm64 kernel, 32-bit user space). The helper is
 * linked statically against glibc 2.39, which tries the syscalls Linux 5.1
 * added for 64-bit time first (clock_gettime64, clock_nanosleep_time64, and
 * statx, also used inside stat, fstat, opendir and fopen) and falls back when
 * they fail with ENOSYS. The 4.4 kernel logs every such call with a register
 * dump, and printing it costs real CPU time. Code that runs every second or
 * more often uses these functions, which make the 4.4 syscalls directly. */
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

struct ub_stat { unsigned long long dev,ino; long long size,mtime; unsigned mode,uid,gid; };
struct ub_dir { int fd,pos,len,err; char buf[4096] __attribute__((aligned(8))); };

/* CLOCK_MONOTONIC */
void ub_now(struct timespec *t);
unsigned long long ub_now_ms(void);
void ub_sleep_ms(unsigned ms);
int ub_stat(const char *path,struct ub_stat *s);
int ub_fstat(int fd,struct ub_stat *s);
/* Directory entries without opendir; names include "." and "..". */
int ub_dir_open(struct ub_dir *d,const char *path);
/* 1 and *name: the next entry; 0: the end of the directory; -1: it could not
 * be read completely (errno, also kept in d->err; every later call fails too).
 * An interrupted read is retried. Code that needs the whole list (a thread
 * scan) must treat -1 as an incomplete list. */
int ub_dir_read(struct ub_dir *d,const char **name);
/* The next name, or NULL at the end and after an error (then d->err != 0). */
const char *ub_dir_next(struct ub_dir *d);
void ub_dir_close(struct ub_dir *d);
/* Whole small file, NUL-terminated (instead of fopen); length or -1. */
ssize_t ub_read_file(const char *path,char *buf,size_t size);
/* Helper owns child exit statuses, regardless of its launcher's disposition. */
int ub_child_wait_init(void);
/* Forked children must release locks even after a partial /proc/fd scan. */
int ub_close_inherited(void);
/* keep_stdio: bit 0/1/2 preserves an open stdin/stdout/stderr; others use null. */
int ub_child_sanitize(unsigned keep_stdio);
#endif
