/* OPTIONS menus inside apl_dispsrv (menu.c, a payload): the "External
 * Subtitles" file browser, shown while that module is enabled, and the
 * "UB820 Helper" menu (Blu-ray region, SSH, helper log). The same UB820
 * Helper is a tile in Player Settings > System (settings.c, a payload in
 * apl_setting, which runs only while Player Settings is open; that part is
 * not guarded, since apl_setting ends normally every time). The payload talks to
 * the other modules only through files in /tmp (helper_menu.h, protocol.h,
 * storage.h, ssh.h), so each module keeps working without it and the menus
 * keep working with any of those modules switched off. */
#include "ub.h"
#include "payloads.h"

#define ID "ui"

static struct ub_part parts[]={
 {"menu","apl_dispsrv",UB_SHA256_APL_DISPSRV,&ub_payload_ui_menu,1,0,0,{0},0,0},
 {"settings","apl_setting",UB_SHA256_APL_SETTING,&ub_payload_ui_settings,0,0,0,{0},0,0},
};
struct ub_module ub_module_ui={
 ID,"Menus: OPTIONS (External Subtitles, UB820 Helper) and Player Settings > System > UB820 Helper",
 1,0,0,parts,sizeof parts/sizeof *parts,0
};
