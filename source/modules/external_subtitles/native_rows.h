/* The player's own hidden external-subtitle rows in OPTIONS (option
 * native_options). Included by module.c after ub.h.
 *
 * apl_dispsrv's own ENABLE_BD_SRT_TEXT cache (FUN_001baf28: -1 unread, 0 off,
 * 1 on). With 1, the stock OPTIONS mask builder FUN_001974c0 stops hiding the
 * four native external-text rows (0x3000800 size -> 0x8eb0, 0x3001000 sync ->
 * 0x8eb1, 0x3002000 character code -> 0x8eb4, 0x3004000 on/off -> 0x8eb5)
 * whenever bdplay reports is_srttext. Data-only write into .data, made once the
 * exact executable hash is verified, independent of any payload. */
#ifndef UB_NATIVE_ROWS_H
#define UB_NATIVE_ROWS_H
#define UI_SRT_GATE_ADDR 0x243b80u

/* state[0]: 1 once handled in this process instance */
static void native_rows_step(struct ub_part *p,struct ub_target *t){
 uint32_t value,one=1;
 if(t->state[0])return;
 if(ub_mem_read(t->pid,UI_SRT_GATE_ADDR,&value,4))return;
 if(value!=1u&&value!=0u&&value!=0xffffffffu){t->state[0]=1;ub_log(p->module,UB_ALWAYS,"native rows flag has unexpected value 0x%x in PID%d; left unchanged",value,t->pid);return;}
 if(value!=1u){if(ub_mem_write(t->pid,UI_SRT_GATE_ADDR,&one,4))return;ub_changed(p);}
 t->state[0]=1;ub_log(p->module,UB_INFO,"native subtitle rows enabled in OPTIONS (PID%d)",t->pid);
}
#endif
