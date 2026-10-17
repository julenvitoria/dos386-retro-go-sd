; Regression guest for x86_hle.c hle_retry (2026-10-17). Build: nasm -f bin hook16.asm -o HOOK16.COM,
; put it on a .dsk with AUTOEXEC.BAT "HOOK16", press START at ~10 s, read guest
; linear 0x4F0 with probe.py --guest 0x4F0:4: 33 33 0D 1C = pass, 22 22 = blocked
; forever, guest_halted=1 = the old rewind executed the far pointer as code.
; INT 16h hooked the way Prince of Persia does it: the hook chains to the
; previous handler with PUSHF + CALL FAR, then the program blocks on AH=00.
org 100h
start:
    xor ax, ax
    mov ds, ax
    mov word [ds:4F0h], 0x1111
    push cs
    pop ds
    xor ax, ax
    mov es, ax
    mov ax, [es:16h*4]
    mov [old16], ax
    mov ax, [es:16h*4+2]
    mov [old16+2], ax
    cli
    mov word [es:16h*4], hook
    mov [es:16h*4+2], cs
    sti
    mov word [es:4F0h], 0x2222
    mov ah, 0
    int 16h                 ; blocks until a key arrives
    mov [es:4F2h], ax
    mov word [es:4F0h], 0x3333
spin:
    hlt
    jmp spin
hook:
    pushf
    call far [cs:old16]
    iret
    align 16
marker: dw 0
result: dw 0
old16:  dd 0
