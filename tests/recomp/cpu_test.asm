; Recompiler self test: every check prints one letter if it passes, "!" and the check letter if it fails.
cpu 186
org 100h

%macro PASS 1
    mov dl, %1
    call putc
%endmacro

%macro FAIL 1
    mov dl, '!'
    call putc
    mov dl, %1
    call putc
%endmacro

start:
    mov ax, 0x7fff              ; A: add overflow sets OF and SF, clears CF
    add ax, 1
    jno .a_fail
    jns .a_fail
    jc .a_fail
    PASS 'A'
    jmp .b
.a_fail:
    FAIL 'A'
.b:
    mov al, 0x10                ; B: sub borrow sets CF, cmp equal sets ZF
    sub al, 0x20
    jnc .b_fail
    cmp al, 0xf0
    jne .b_fail
    PASS 'B'
    jmp .c
.b_fail:
    FAIL 'B'
.c:
    mov ax, 0x8001              ; C: shl moves the top bit into CF, rcr brings it back
    shl ax, 1
    jnc .c_fail
    mov bx, ax
    rcr ax, 1
    cmp bx, 0x0002
    jne .c_fail
    cmp ax, 0x8001
    jne .c_fail
    PASS 'C'
    jmp .d
.c_fail:
    FAIL 'C'
.d:
    mov ax, 1234                ; D: mul and div
    mov bx, 100
    mul bx
    cmp ax, 0xe208
    jne .d_fail
    cmp dx, 1
    jne .d_fail
    div bx
    cmp ax, 1234
    jne .d_fail
    PASS 'D'
    jmp .e
.d_fail:
    FAIL 'D'
.e:
    mov di, buffer              ; E: rep stosb, then lodsb reads it back
    mov cx, 8
    mov al, 0x5a
    cld
    rep stosb
    mov si, buffer + 7
    lodsb
    cmp al, 0x5a
    jne .e_fail
    cmp di, buffer + 8
    jne .e_fail
    PASS 'E'
    jmp .f
.e_fail:
    FAIL 'E'
.f:
    xor bx, bx                  ; F: loop runs CX times
    mov cx, 10
.f_loop:
    inc bx
    loop .f_loop
    cmp bx, 10
    jne .f_fail
    PASS 'F'
    jmp .g
.f_fail:
    FAIL 'F'
.g:
    mov ax, 1                   ; G: pusha/popa restore registers, nested call returns
    mov bp, 2
    pusha
    mov ax, 0
    mov bp, 0
    call nested
    popa
    cmp ax, 1
    jne .g_fail
    cmp bp, 2
    jne .g_fail
    PASS 'G'
    jmp .h
.g_fail:
    FAIL 'G'
.h:
    mov al, -8                  ; H: signed compare and sar
    cmp al, 3
    jge .h_fail
    sar al, 1
    cmp al, -4
    jne .h_fail
    PASS 'H'
    jmp .i
.h_fail:
    FAIL 'H'
.i:
    mov si, text                ; I: repne scasb finds a byte
    mov di, text
    mov cx, 10
    mov al, 'q'
    repne scasb
    jne .i_fail
    cmp di, text + 4
    jne .i_fail
    PASS 'I'
    jmp .done
.i_fail:
    FAIL 'I'
.done:
    mov dl, 13
    call putc
    mov dl, 10
    call putc
    mov ax, 0x4c00
    int 0x21

nested:
    call inner
    ret
inner:
    ret

putc:
    push ax
    mov ah, 2
    int 0x21
    pop ax
    ret

text: db "abcqdefghi"
buffer: times 8 db 0
