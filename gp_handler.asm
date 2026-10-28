.code _text

EXTERN original_gp_handler:QWORD
EXTERN gp_handler:PROC

.CODE

; stack frame
; [rsp + 0]  error_code
; [rsp + 8]  rip
; [rsp + 16] cs 0 = kernel, 3 = user
; [rsp + 24] rflags
; [rsp + 32] rsp
; [rsp + 40] ss
PUBLIC gp_handler_stub  
gp_handler_stub PROC
	test byte ptr[rsp + 16], 3
    jnz is_user_mode
	jmp qword ptr [original_gp_handler];    cpl is 0, not our exception, let the original handler handle it

is_user_mode:
    swapgs ;    switch to kernel gs

	push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    mov rbp, rsp

    mov rcx, rbp ; pass regs as the first argument to gp_handler
    mov rdx, [rbp+80h] ; move faulting rip to rdx

    mov rax, 00007FFFFFFFFFFEh 
    cmp rdx, rax 
    ja not_cpuid ;  rip is a kernel space address attempted to execute in usermode, resulting in #GP, not our exception

    stac ;  disable smap to read an usermode page in kernelmode
    movzx r8d, word ptr [rdx]
    clac ;  re-enable smap
    cmp r8d, 0A20Fh ;   check if faulting rip is cpuid instruction, if not its not our exception, let the original handler handle it
    jne not_cpuid

    sub rsp, 20h
    call gp_handler ;   handle cpuid call
    add rsp, 20h

    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    swapgs ;    swap back to usermode gs
    add rsp, 8 ;    remove error code from stack frame
    iretq ;     return from interrupt

not_cpuid:
    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    swapgs ;    swap back to usermode gs as KiGeneralProtectionFault will swap it again
    jmp qword ptr [original_gp_handler]
gp_handler_stub ENDP

END