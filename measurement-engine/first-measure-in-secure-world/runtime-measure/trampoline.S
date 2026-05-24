#define ASSEMBLY
.section .trampoline,"ax"
.global __collect__icall
.global __collect__ijump
.global __collect__conditional_branch_pred
.global __collect_indirect_call_pred
.global __collect_indirect_jump_pred
.global __collect_ldmia_ret
.global __str_sfi
.global __error_handle
.global __str_sfi_backup

@enable shadow stack
.global __collect_bx_ret
.global __collect_direct_call_pred
.global __collect_direct_call

__collect_bx_ret:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    bl collect_bx_lr
    mov r8,#0
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {pc}

__collect_direct_call_pred:
    push {lr}
    push {r12}
    mov r2,lr
    bl collect_bl_pred
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    mov pc,r8

__collect_direct_call:
    push {lr}
    push {r0}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    mov r0,lr
    bl collect_bl
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {r8}
    mov pc,r8

@disable shadow stack
__collect__conditional_branch_pred:
    push {r0,r2, r3, r12, lr}
    bl collect_bcc
    pop {r0}
    msr cpsr_f, r0
    pop {r2, r3, r12, pc}
    
__collect__icall:
    push {r1,r2, r3, r12, lr}
    bl collect_blx
    mov r8,#0
    pop {r1,r2, r3, r12, pc}



__collect__ijump:
    push {lr}
    push {r1}
    push {r2}
    push {r3}
    push {r12}
    bl collect_bx
    pop {r12}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {pc}



__collect_indirect_call_pred:
    push {lr}
    push {r12}
    bl collect_indirect_call_pred
    pop {r12}
    pop {pc}




__collect_indirect_jump_pred:
    push {lr}
    push {r12}
    bl collect_indirect_jump_pred
    pop {r12}
    pop {r8}
    pop {r3}
    pop {r2}
    pop {r1}
    pop {r0}
    pop {lr}
    mov pc,r8

__collect_ldmia_ret:
    push {r0,r1,r2, r3, r12, lr}
    mov r0,r8
    push {r8}
    bl collect_ldmia_ret
    pop {r8}
    pop {r0,r1,r2, r3, r12, pc}
    


__str_sfi:
    push    {r3,r8}
    mrs r3, cpsr
    movw    r8, #0x0000
    movt    r8, #0x102          @ r8 = 0x1020000
    sub     r8, r0, r8
    cmp     r8, #0x30000
    blo     abort
    b       safe_exit

__str_sfi_backup:
    push {r3}
    push {r8}              
    mrs r3, cpsr
    movw r8, #0x0     
    movt r8, #0x102       
    cmp  r0, r8             
    blt  safe_exit         
    @movw r8, #0x000000     
    movt r8, #0x105    
    cmp  r0, r8            
    blt  abort           
    b    safe_exit
safe_exit:
    msr cpsr_f, r3
    pop {r3,r8}             
    bx lr               

__error_handle:
    bl abort             
