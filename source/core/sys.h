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

struct ub_stat { unsigned long long dev,ino; long long size,mtime; unsigned mode; };
struct ub_dir { int fd,pos,len; char buf[4096] __attribute__((aligned(8))); };

/* CLOCK_MONOTONIC */
void ub_now(struct timespec *t);
unsigned long long ub_now_ms(void);
void ub_sleep_ms(unsigned ms);
int ub_stat(const char *path,struct ub_stat *s);
int ub_fstat(int fd,struct ub_stat *s);
/* Directory entries without opendir; names include "." and "..". */
int ub_dir_open(struct ub_dir *d,const char *path);
const char *ub_dir_next(struct ub_dir *d);
void ub_dir_close(struct ub_dir *d);
/* Whole small file, NUL-terminated (instead of fopen); length or -1. */
ssize_t ub_read_file(const char *path,char *buf,size_t size);
#endif
