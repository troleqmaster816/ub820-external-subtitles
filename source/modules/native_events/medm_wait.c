/* Installed in the SAME transaction as the media-manager launcher. This
 * covers subsequent public API entries at the seven audited PCA/FLC waits.
 * A call already inside libev during that first stop cannot be attested or
 * repaired by an entry hook; retain this explicit boot-daemon limitation. */
#include <errno.h>
#include <stdint.h>
#include "wait_scope.h"
int (*ub_original_medm_wait)(uint32_t,void *);
__attribute__((noinline)) int ub_hook_medm_wait(uint32_t queue,void *out){
 uintptr_t caller=(uintptr_t)__builtin_return_address(0);int saved=errno,rc;
 if(!native_medm_wait_site(caller))return ub_original_medm_wait(queue,out);
 do{errno=saved;rc=ub_original_medm_wait(queue,out);}while(rc==EINTR);
 if(!rc)errno=saved;
 return rc;
}
