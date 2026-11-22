; Original x86 helper: verify peer, then cancel pending I/O on that socket.
; No game code, import table, thread context, or authorization path is changed.
.386
.model flat
.code
PUBLIC _RemoteBegin
PUBLIC _RemoteEnd
_RemoteBegin LABEL BYTE
    push ebx
    mov ebx, dword ptr [esp+8]
    lea eax, [ebx+24]
    lea edx, [ebx+20]
    push edx
    push eax
    push dword ptr [ebx]
    call dword ptr [ebx+4]
    test eax, eax
    jnz socket_error
    cmp word ptr [ebx+24], 2
    jne peer_mismatch
    mov ax, word ptr [ebx+16]
    cmp word ptr [ebx+26], ax
    jne peer_mismatch
    cmp dword ptr [ebx+28], 0100007fh
    jne peer_mismatch
    push 0
    push dword ptr [ebx]
    call dword ptr [ebx+8]
    mov dword ptr [ebx+44], eax
    test eax, eax
    jz api_error
    xor eax, eax
    jmp finished
api_error:
    call dword ptr [ebx+12]
    mov dword ptr [ebx+48], eax
    mov eax, 1
    jmp finished
socket_error:
    call dword ptr [ebx+52]
    mov dword ptr [ebx+48], eax
    mov eax, 3
    jmp finished
peer_mismatch:
    mov eax, 2
finished:
    mov dword ptr [ebx+40], eax
    pop ebx
    ret 4
_RemoteEnd LABEL BYTE
END
