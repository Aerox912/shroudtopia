OPTION CASEMAP:NONE
.code
PUBLIC detour_fixture_target
detour_fixture_target PROC
    mov eax, ecx
    add eax, 7
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ret
detour_fixture_target ENDP
END
