#include <stdlib.h>
#include <unistd.h>
__attribute__((constructor)) static void inherited_init(void){
 if(setenv("FIXTURE_OLD_CTOR","1",1))_exit(93);
}
