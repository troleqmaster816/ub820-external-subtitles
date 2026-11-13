/* A64 exact-site bridge: nonzero x19 is fully transparent. Zero native
 * continuation kills caller scratch/NZCV before observation (V10 proof).
 *16-byte FP/LR record; no task flags, IRQ or preempt state changes. */
.text
.p2align 3
.global ub_fpd_wait_bridge_v15
.type ub_fpd_wait_bridge_v15,%function
ub_fpd_wait_bridge_v15:
 cbz x19,1f
 b ub_fpd_wait_fallthrough_v15
1:
 stp x29,x30,[sp,#-16]!
 mov x29,sp
 ldr x0,2f
 bl ub_cpu_wait_v15
 ldp x29,x30,[sp],#16
 b ub_fpd_wait_loop_v15
.size ub_fpd_wait_bridge_v15,.-ub_fpd_wait_bridge_v15
.p2align 3
2: .xword ub_fpd_wait_context_v15
.p2align 3
.global ub_antena_wait_bridge_v15
.type ub_antena_wait_bridge_v15,%function
ub_antena_wait_bridge_v15:
 cbz x19,3f
 b ub_antena_wait_fallthrough_v15
3:
 stp x29,x30,[sp,#-16]!
 mov x29,sp
 ldr x0,4f
 bl ub_cpu_wait_v15
 ldp x29,x30,[sp],#16
 b ub_antena_wait_loop_v15
.size ub_antena_wait_bridge_v15,.-ub_antena_wait_bridge_v15
.p2align 3
4: .xword ub_antena_wait_context_v15
.section .note.GNU-stack,"",%progbits
