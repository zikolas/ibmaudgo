;==========================================================================
;  TRTEST2 - string-I/O check for IBMTRJ.  Run with and without the trap;
;  every line must match.
;
;    REP INSW  x4 from 346h (read-only position counter)   CX, DI advance
;    REP OUTSW x16 of 8080h to 340h (silence, as the vendor driver primes
;              the FIFO)                                    CX, SI advance
;    STD REP INSB x4 from 346h                              CX, DI advance
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
;==========================================================================
        cpu  386
        org  100h

        push ds
        pop  es
        cld

        mov  si, m_insw
        call prstr
        mov  di, buf
        mov  cx, 4
        mov  dx, 346h
        rep  insw
        call report_di
        mov  si, m_crlf
        call prstr

        mov  si, m_outsw
        call prstr
        mov  si, silence
        mov  cx, 16
        mov  dx, 340h
        rep  outsw
        mov  ax, cx
        call prhex4
        mov  al, ' '
        call prchar
        mov  ax, si
        sub  ax, silence
        call prhex4
        mov  si, m_crlf
        call prstr

        mov  si, m_std
        call prstr
        mov  di, buf+7
        mov  cx, 4
        mov  dx, 346h
        std
        rep  insb
        cld
        mov  ax, cx
        call prhex4
        mov  al, ' '
        call prchar
        mov  ax, di
        sub  ax, buf+7
        call prhex4
        mov  si, m_crlf
        call prstr

        mov  ax, 4C00h
        int  21h

; CX and DI-buf, then the four words read
report_di:
        mov  ax, cx
        call prhex4
        mov  al, ' '
        call prchar
        mov  ax, di
        sub  ax, buf
        call prhex4
        ret

prhex4:
        push ax
        mov  al, ah
        call prhex2
        pop  ax
prhex2:
        push ax
        shr  al, 4
        call prnyb
        pop  ax
prnyb:
        push ax
        and  al, 0Fh
        add  al, '0'
        cmp  al, '9'
        jbe  .o
        add  al, 7
.o:
        call prchar
        pop  ax
        ret
prchar:
        push ax
        push dx
        mov  dl, al
        mov  ah, 2
        int  21h
        pop  dx
        pop  ax
        ret
prstr:
        push ax
.l:
        lodsb
        or   al, al
        jz   .r
        call prchar
        jmp  .l
.r:
        pop  ax
        ret

m_insw  db 'rep insw  346 x4:  cx/di ',0
m_outsw db 'rep outsw 340 x16: cx/si ',0
m_std   db 'std rep insb 346 x4: cx/di ',0
m_crlf  db 13,10,0
silence times 16 dw 8080h
buf     times 8 db 0
