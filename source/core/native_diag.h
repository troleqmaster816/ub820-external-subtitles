#ifndef UB_NATIVE_DIAG_H
#define UB_NATIVE_DIAG_H
/* log.diagnostics requests native EV error details for future BD launches.
 * The media-manager launcher reads the RAM marker; no live environment is
 * rewritten and no additional target thread is stopped for this observation. */
#ifndef UB_NATIVE_EV_DEBUG
#define UB_NATIVE_EV_DEBUG "/tmp/ub820_native_ev_debug"
#endif
#define UB_NATIVE_EV_DEBUG_VALUE "EV1\n"
#endif
