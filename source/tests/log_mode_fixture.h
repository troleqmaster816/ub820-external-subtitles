/* Host fixture for the menus: the log module is enabled, i.e. its mode file
 * (UB_LOG_MODE) exists, at a private temporary path removed at exit. A test
 * unlinks log_mode_path to model a disabled log module. Include before the
 * menu payload source or helper_actions.h. */
#ifndef UB_LOG_MODE_FIXTURE_H
#define UB_LOG_MODE_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "helper_menu.h"
static char log_mode_path[64];
#undef UB_LOG_MODE
#define UB_LOG_MODE log_mode_path
__attribute__((constructor)) static void log_mode_fixture_start(void){
 int fd;snprintf(log_mode_path,sizeof log_mode_path,"/tmp/ub820-log-mode-XXXXXX");
 fd=mkstemp(log_mode_path);
 if(fd<0||write(fd,"manual\n",7)!=7||close(fd)){perror("log mode fixture");exit(2);}
}
__attribute__((destructor)) static void log_mode_fixture_end(void){unlink(log_mode_path);}
#endif
