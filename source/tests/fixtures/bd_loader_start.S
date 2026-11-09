.syntax unified
.thumb
.text
.align 2
.global _start
.type _start,%function
.thumb_func
_start:
 mov r4,r0
 /* This check is the executable's first action, before libc_start_main. */
 bl fixture_entry_check
 ldr r1,[sp],#4
 mov r2,sp
 push {r2}
 push {r4}
 movs r3,#0
 push {r3}
 ldr r0,=main
 bl __libc_start_main
 b .
.size _start,.-_start
.align 2
.global region_source0
.type region_source0,%function
.thumb_func
region_source0:
 movs r3,#1
 mov r0,r3
 bx lr
.global region_source1
.type region_source1,%function
.thumb_func
region_source1:
 movs r3,#2
 mov r0,r3
 bx lr
.global region_source2
.type region_source2,%function
.thumb_func
region_source2:
 movs r3,#4
 mov r0,r3
 bx lr
.section .note.GNU-stack,"",%progbits
