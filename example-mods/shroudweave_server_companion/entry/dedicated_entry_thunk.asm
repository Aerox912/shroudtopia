option casemap:none
.code
IFDEF ESM_DEDICATED_FIXTURE_EXE
EXTERN EsmFixtureEntered:PROC
PUBLIC FixtureEntry
FixtureEntry PROC FRAME
    pushfq
    .allocstack 8
    push rax
    .pushreg rax
    push rcx
    .pushreg rcx
    push rdx
    .pushreg rdx
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r8
    .pushreg r8
    push r9
    .pushreg r9
    push r10
    .pushreg r10
    push r11
    .pushreg r11
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 528h
    .allocstack 528h
    .endprolog
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    movdqu [rsp+60h], xmm4
    movdqu [rsp+70h], xmm5
    movdqu [rsp+80h], xmm6
    movdqu [rsp+90h], xmm7
    movdqu [rsp+0a0h], xmm8
    movdqu [rsp+0b0h], xmm9
    movdqu [rsp+0c0h], xmm10
    movdqu [rsp+0d0h], xmm11
    movdqu [rsp+0e0h], xmm12
    movdqu [rsp+0f0h], xmm13
    movdqu [rsp+100h], xmm14
    movdqu [rsp+110h], xmm15
    stmxcsr [rsp+120h]
    lea rdi, [rsp+1bfh]
    and rdi, -64
    mov [rsp+128h], rdi
    mov r10, rdi
    mov ecx, 832
    xor eax, eax
    rep stosb
    xor ecx, ecx
    xgetbv
    and eax, 7
    xor edx, edx
    xsave64 [r10]
    mov rcx, rsp
    call EsmFixtureEntered
    int 29h
FixtureEntry ENDP
PUBLIC EsmFixtureClobberXstate
EsmFixtureClobberXstate PROC
    vpcmpeqd ymm0, ymm0, ymm0
    vpcmpeqd ymm1, ymm1, ymm1
    vpcmpeqd ymm2, ymm2, ymm2
    vpcmpeqd ymm3, ymm3, ymm3
    vpcmpeqd ymm4, ymm4, ymm4
    vpcmpeqd ymm5, ymm5, ymm5
    vpcmpeqd ymm6, ymm6, ymm6
    vpcmpeqd ymm7, ymm7, ymm7
    vpcmpeqd ymm8, ymm8, ymm8
    vpcmpeqd ymm9, ymm9, ymm9
    vpcmpeqd ymm10, ymm10, ymm10
    vpcmpeqd ymm11, ymm11, ymm11
    vpcmpeqd ymm12, ymm12, ymm12
    vpcmpeqd ymm13, ymm13, ymm13
    vpcmpeqd ymm14, ymm14, ymm14
    vpcmpeqd ymm15, ymm15, ymm15
    ret
EsmFixtureClobberXstate ENDP
PUBLIC EsmFixtureCallSite
EsmFixtureCallSite PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
PUBLIC EsmFixtureCommitSite
EsmFixtureCommitSite LABEL BYTE
    db 0e8h, 6, 7fh, 0, 0
    add rsp, 28h
    ret
    db 7f01h dup (0cch)
    ret
EsmFixtureCallSite ENDP
ELSE
EXTERN EsmDedicatedStartup:PROC
EXTERN EsmDedicatedRefused:PROC
PUBLIC EsmDedicatedEntryThunk
PUBLIC EsmDedicatedRefusalThunk
EsmDedicatedEntryThunk PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    call EsmDedicatedStartup
    int 29h
EsmDedicatedEntryThunk ENDP
EsmDedicatedRefusalThunk PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    call EsmDedicatedRefused
    int 29h
EsmDedicatedRefusalThunk ENDP
ENDIF
END
