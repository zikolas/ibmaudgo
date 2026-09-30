;==========================================================================
;  TRTEST - pass-through check for IBMTRACE.
;
;  Run once without IBMTRACE and once with it; the latch lines must match.
;  Card configured by IBMAUDGO at 250h/340h.  Touches only 0x258/0x259
;  (plain latches: they return what was written) and restores both; the
;  rest is reads.
;
;    byte IN  250-25F        (IBMTRACE: QPI 1A00h path)
;    word IN  340/342/344/346 (untrap / exact word / retrap path)
;    byte OUT 258 <- 5A, read back           (QPI 1A01h path)
;    word OUT 258 <- 3C5A, read 258 and 259  (untrap / word / retrap path)
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
;==========================================================================
        cpu  386
        org  100h

        mov  dx, 258h
        in   al, dx
        mov  [s258], al
        inc  dx
        in   al, dx
        mov  [s259], al

        mov  si, m_b
        call prstr
        mov  dx, 250h
.b:
        in   al, dx
        call prhex2
        mov  al, ' '
        call prchar
        inc  dx
        cmp  dx, 260h
        jb   .b
        mov  si, m_crlf
        call prstr

        mov  si, m_w
        call prstr
        mov  dx, 340h
.w:
        in   ax, dx
        push ax
        mov  al, ah
        call prhex2
        pop  ax
        call prhex2
        mov  al, ' '
        call prchar
        add  dx, 2
        cmp  dx, 348h
        jb   .w
        mov  si, m_crlf
        call prstr

        mov  si, m_l1
        call prstr
        mov  dx, 258h
        mov  al, 5Ah
        out  dx, al
        in   al, dx
        call prhex2
        mov  si, m_crlf
        call prstr

        mov  si, m_l2
        call prstr
        mov  dx, 258h
        mov  ax, 3C5Ah
        out  dx, ax
        in   al, dx
        call prhex2
        mov  al, ' '
        call prchar
        inc  dx
        in   al, dx
        call prhex2
        mov  si, m_crlf
        call prstr

        mov  dx, 258h
        mov  al, [s258]
        out  dx, al
        inc  dx
        mov  al, [s259]
        out  dx, al
        mov  ax, 4C00h
        int  21h

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
prhex2:
        push ax
        shr  al, 4
        call prnyb
        pop  ax
        push ax
        call prnyb
        pop  ax
        ret
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

s258    db 0
s259    db 0
m_b     db 'byte 250-25F: ',0
m_w     db 'word 340-346: ',0
m_l1    db 'latch 258<-5A reads   ',0
m_l2    db 'latch 258<-3C5A reads ',0
m_crlf  db 13,10,0
