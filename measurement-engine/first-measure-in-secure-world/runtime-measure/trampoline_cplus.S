#define ASSEMBLY
.section .trampoline,"ax"
.global __collect__icall
.global __collect__icall_shadow_stack
.global __collect__ijump
.global __collect_bx_ret
.global __collect_bx_ret_shadow_stack
.global __collect_indirect_call_pred
.global __collect_indirect_jump_pred
.global __collect_indirect_call_pred_shadow_stack
.global __collect_ldmia_ret
.global __collect_ldmia_ret_shadow_stack
.global __collect_direct_call_pred
.global __collect_direct_call




__collect__icall:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r1,lr
    bl _Z11collect_blxjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r8}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect__icall_shadow_stack:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r1,lr
    bl _Z24collect_blx_shadow_stackjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r8}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect__ijump:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r1,lr
    bl _Z10collect_bxjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r8}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_bx_ret:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r1,lr
    bl _Z13collect_bx_lrjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r8}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_bx_ret_shadow_stack:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r1,lr
    bl _Z26collect_bx_lr_shadow_stackjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r8}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_indirect_call_pred:
    push {lr}
    push {r12}
    mov r3,lr
    bl _Z26collect_indirect_call_predjjjj
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_indirect_call_pred_shadow_stack:
    push {lr}
    push {r12}
    mov r3,lr
    bl _Z39collect_indirect_call_pred_shadow_stackjjjj
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_indirect_jump_pred:
    push {lr}
    push {r12}
    mov r3,lr
    bl _Z26collect_indirect_jump_predjjjj
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_ldmia_ret:
    push {lr}
    push {r0}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r0,lr
    mov r1,r8
    bl _Z17collect_ldmia_retjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {pc}

__collect_ldmia_ret_shadow_stack:
    push {lr}
    push {r0}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r0,lr
    mov r1,r8
    bl _Z30collect_ldmia_ret_shadow_stackjj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {pc}

__collect_direct_call_pred:
    push {lr}
    mov r2,lr
    push {r12}
    bl _Z15collect_bl_predjjj
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_direct_call:
    push {lr}
    push {r0}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r0,lr
    bl _Z10collect_blj
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {r8}
    pop {lr}
    mov pc,r8