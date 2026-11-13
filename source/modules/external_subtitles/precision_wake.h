#ifndef UB_SUBTITLE_PRECISION_WAKE_H
#define UB_SUBTITLE_PRECISION_WAKE_H
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

/* One resident worker owns initialization/draining. Native hooks publish a
 * generation and a nonblocking eventfd wake. The descriptor is never closed
 * or reused while hooks can run. Checking generation before poll plus a
 * level-triggered counter prevents a wake between predicate and sleep being
 * lost. A failed allocation retains the old finite-wait fallback. */
struct ub_precision_wake {int fd;uint32_t generation;};
static inline void ub_precision_wake_init(struct ub_precision_wake *w){
 int fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
 if(fd>=0)__atomic_store_n(&w->fd,fd,__ATOMIC_RELEASE);
}
static inline uint32_t ub_precision_wake_generation(struct ub_precision_wake *w){
 return __atomic_load_n(&w->generation,__ATOMIC_ACQUIRE);
}
static inline int ub_precision_wake_fd(struct ub_precision_wake *w){
 return __atomic_load_n(&w->fd,__ATOMIC_ACQUIRE);
}
static inline void ub_precision_wake_signal(struct ub_precision_wake *w){
 int saved=errno,fd;uint64_t one=1;ssize_t n;
 __atomic_add_fetch(&w->generation,1,__ATOMIC_RELEASE);
 fd=ub_precision_wake_fd(w);
 if(fd>=0){do{n=write(fd,&one,sizeof(one));}while(n<0&&errno==EINTR);}
 errno=saved;
}
static inline void ub_precision_wake_drain(struct ub_precision_wake *w){
 int fd=ub_precision_wake_fd(w);uint64_t count;ssize_t n;
 if(fd<0)return;
 do {n=read(fd,&count,sizeof(count));}while(n==(ssize_t)sizeof(count)||(n<0&&errno==EINTR));
}
#endif
