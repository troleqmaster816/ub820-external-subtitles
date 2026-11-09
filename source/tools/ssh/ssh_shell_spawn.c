/* Keep libc system()/popen() inside the SSH tools' normal RAM shell.
 * GNU ld --wrap=posix_spawn redirects musl's existing spawn calls; libc still
 * owns pipes, file actions, signal attributes, cancellation and wait semantics.
 * Only the default /bin/sh path changes. Explicit other programs are untouched.
 * This applies solely to bundled static tools, never to the player's libc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <spawn.h>
#include <string.h>
#ifndef UB_SSH_SHELL
#define UB_SSH_SHELL "/tmp/.ub820_ssh/bin/sh"
#endif
int __real_posix_spawn(pid_t *,const char *,const posix_spawn_file_actions_t *,const posix_spawnattr_t *,char *const[],char *const[]);
int __wrap_posix_spawn(pid_t *,const char *,const posix_spawn_file_actions_t *,const posix_spawnattr_t *,char *const[],char *const[]);
int __wrap_posix_spawn(pid_t *pid,const char *path,const posix_spawn_file_actions_t *actions,
                       const posix_spawnattr_t *attributes,char *const argv[],char *const envp[]){
 if(path && !strcmp(path,"/bin/sh"))path=UB_SSH_SHELL;
 return __real_posix_spawn(pid,path,actions,attributes,argv,envp);
}
