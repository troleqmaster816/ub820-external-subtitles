#ifndef UB_HELPER_MENU_H
#define UB_HELPER_MENU_H
/* Files shared between the "UB820 Helper" menu (modules/ui/menu.c, inside
 * apl_dispsrv) and the helper's modules. Requests are written atomically by
 * the menu (temp file + rename) and consumed by the helper; status files are
 * replaced atomically by the helper. All in /tmp (RAM). New region/remote
 * clients use action_ipc.h: REQUEST.q.<random-session>.<seq> and private
 * REQUEST.r.<random-session>.<seq> replies carrying the status-line format.
 * Global STATUS is observational; it never acknowledges a new client. */

/* Present while the module is enabled; the menu shows its rows only then. */
#define UB_EXTSUB_ENABLED "/tmp/ub820_extsub.enabled"
/* (UB_SSH_ENABLED is in modules/ssh/module.c and read by the menu.) */

/* log module: "always", "menu" or "off" (the [log] save setting). */
#define UB_LOG_MODE "/tmp/ub820_log.mode"

/* region module.
 * request: "set <choice> <seq>\n", choice = auto, stock, A, B, C or ABC.
 * status:  "<seq> <choice> <disc> <player> <message>\n": seq of the last
 *          request handled (0 = none), the current choice, the region value
 *          the current Blu-ray sees (PSR20: 1 A, 2 B, 4 C, 7 all; 0 = no disc
 *          or not known yet), the player's own region (same values, 0 = not
 *          known yet), and a short message about the last request. The
 *          disc's own regions are not known in general (BD-J discs). */
#define UB_REGION_REQUEST "/tmp/ub820_region.request"
#define UB_REGION_STATUS "/tmp/ub820_region.status"
/* region module, DVD: request "set <choice> <seq>\n" (choice = stock, auto,
 * free or 1..8), status "<seq> <choice> <message>\n". */
#define UB_DVD_REQUEST "/tmp/ub820_dvd.request"
#define UB_DVD_STATUS "/tmp/ub820_dvd.status"

/* remote module.
 * request: "set <choice> <seq>\n", choice = netflix, nothing or helper.
 * status:  "<seq> <choice> <message>\n" (seq of the last request handled).
 * UB_NETFLIX_OFF exists while the Netflix button does nothing, and
 * UB_NETFLIX_HELPER while it opens the UB820 Helper overlay; the payload in
 * apl_mgr checks them on each press. */
#define UB_REMOTE_REQUEST "/tmp/ub820_remote.request"
#define UB_REMOTE_STATUS "/tmp/ub820_remote.status"
#define UB_NETFLIX_OFF "/tmp/ub820_netflix.off"
#define UB_NETFLIX_HELPER "/tmp/ub820_netflix.helper"

/* power module: idle auto-power-off only. The enabled marker is published
 * only while the supported native policy control is ready. Requests use
 * private action envelopes: "set on|off <seq>\n". Status and replies contain
 * "<seq> <on|off> <message>\n"; the value is the confirmed effective state,
 * including the previous state when applying a request failed. */
#define UB_POWER_ENABLED "/tmp/ub820_power.enabled"
#define UB_POWER_REQUEST "/tmp/ub820_power.request"
#define UB_POWER_STATUS "/tmp/ub820_power.status"

/* cpu module: the enabled marker controls visibility; it does not imply that
 * optimization is On. Requests use "set on|off <seq>\n" or the page-open
 * query "get <seq>\n" in private action envelopes. Replies contain
 * "<seq> <on|off|unknown> <message>\n". Only the backend confirms state. */
#define UB_CPU_ENABLED "/tmp/ub820_cpu.enabled"
#define UB_CPU_REQUEST "/tmp/ub820_cpu.request"
#define UB_CPU_STATUS "/tmp/ub820_cpu.status"

/* UB820 Helper overlay (ui module, apl_dispsrv), opened by the Netflix key.
 * UB_KEYS_FIFO (made by the remote module) carries key presses from apl_mgr
 * as u32 key codes; the overlay keeps it open for reading. UB_OVERLAY_OPEN
 * exists while the overlay is shown: apl_mgr then sends it every press and
 * drops the keys for everything else, as long as somebody reads the FIFO. */
#define UB_KEYS_FIFO "/tmp/ub820_keys.fifo"
#define UB_OVERLAY_OPEN "/tmp/ub820_overlay.open"
/* OPEN contains a u32 CLOCK_MONOTONIC expiry (seconds), refreshed by UI.
 * ABORT requests closure after a failed FIFO delivery; never execute that
 * same key against the application hidden underneath the overlay. */
#define UB_OVERLAY_ABORT "/tmp/ub820_overlay.abort"
#endif
