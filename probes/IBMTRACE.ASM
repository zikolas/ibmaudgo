;==========================================================================
;  IBMTRACE - I/O trace of the IBM PCMCIA Audio Adapter's two port blocks.
;
;  A QPI port-trap TSR (Jemm + QPIEMU, or QEMM).  It traps the card's
;  16-byte block (default 250h) and 8-byte block (default 340h), performs
;  every access on the real hardware exactly as the guest issued it, and
;  logs it.  Load it, let the vendor driver play a WAV, then write the log
;  to a file and decode it on the host (probes/ibmtrace.py).
;
;    IBMTRACE [/IO1=250] [/IO2=340] [/K=128] [/N=port ...] [/D=n] [/Q]
;                                install, armed
;    IBMTRACE /C [/N=port ...]   clear log + counters, re-arm (new filters)
;    IBMTRACE /STOP              stop logging (counters stop too)
;    IBMTRACE /S                 summary: per-port counts
;    IBMTRACE /W=file            stop logging, write header + log to file
;    IBMTRACE /U                 untrap, restore the previous handler, free
;
;  /K   log size in KB (16..320), 12-byte entries, filled from the start;
;       once full, later events are only counted.  An entry that repeats the
;       previous one exactly (port, value, access type) bumps its repeat
;       count instead of taking a new slot, so status polling stays cheap.
;  /N   do not log this port (it is still counted); up to 4.
;  /D   IBMTRJ only: string elements logged per operation (default 0 = all).
;  /Q   use the QPI (V86) trap even when IBMTRJ is loaded.
;
;  Two backends.  If the IBMTRJ JLM is loaded (JLOAD IBMTRJ.DLL) the ports
;  are trapped at ring 0 by it: exact widths, and REP string I/O performed
;  in full with its count and data logged.  Otherwise a QPI (V86) handler
;  here does the trapping - it cannot see a REP count (below).
;
;  Entry (12 bytes): port w, value w, QPI access type b, repeats-1 b,
;  BIOS tick low word w, PIT ch0 count w, PIT ch0 status b, 0 b.  Count
;  and status come from one 8254 read-back, so the status OUT bit says
;  which half of a mode-3 period the count belongs to.
;
;  QPI access type (CL): bit 2 OUT, bit 3 word, bit 4 dword, bit 5 string,
;  bit 6 REP.  QPIEMU passes string I/O straight through to this handler
;  and overwrites the guest's CX with the type, so the REP count is lost:
;  a REP string op is performed ONCE and counted in "rep lost".  If that
;  counter moves, the driver uses REP INS/OUTS on the card and the trace
;  shows on which port.
;
;  Runs in the guest's V86 context under CLI, so it is not re-entered.  It
;  touches only the card's two blocks and the PIT latch (43h/40h); never
;  the PCIC and never the COMrade link.  The pass-through never uses a
;  plain IN/OUT on a trapped port (see trap_handler).
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
;  Original work.  The QPI handler contract is the public QEMM interface;
;  QPIEMU's string-I/O behaviour was read from Jemm's public source.
;==========================================================================
        cpu  386
        org  100h

NPORT   equ 24                        ; 16 + 8
NKIND   equ 5                         ; in b, in w, out b, out w, other
ENTSZ   equ 12

start:
        jmp  init

; ---- resident header: FIXED layout, the host decoder reads it ----------
sig       db 'IBTR'                   ; 103h
hver      db 3                        ; 107h
armed     db 0                        ; 108h
nfilt     db 0                        ; 109h
w0        dw 0250h                    ; 10Ah
w1        dw 0340h                    ; 10Ch
filt      dw 0, 0, 0, 0               ; 10Eh
oldtrap   dd 0                        ; 116h  QPI handler before us
bufseg0   dw 0                        ; 11Ah
bufcap    dd 0                        ; 11Ch  entries
wrptr     dw 0, 0                     ; 120h  off, seg
nlog      dd 0                        ; 124h  entries used
nevents   dd 0                        ; 128h  accesses to our ports
ndrop     dd 0                        ; 12Ch  not logged: log full
nrep      dd 0                        ; 130h  REP string ops (count lost)
nchain    dd 0                        ; 134h  passed to the previous handler
counts    times NPORT*NKIND dd 0      ; 138h  [port][kind]
lastp     dw 0, 0                     ; 318h  last entry (off, seg)
nfail     dd 0                        ; 31Ch  QPI untrap/retrap refusals
strmax    dw 0                        ; 320h  IBMTRJ: string elements logged
                                      ;       per operation, 0 = all
backend   db 0                        ; 322h  0 = QPI (V86), 1 = IBMTRJ
hdr_end:                              ; 323h

qpi       dd 0                        ; QPI entry, for the pass-through
tmpval    dw 0
tmpkind   dw 0
tmpidx    dw 0
tmpn      dw 0
tmpfn     dw 0

; ---- QPI trap handler ----------------------------------------------------
; DX = port, CL = access type, AX/EAX = value on OUT.  RETF; for a
; non-string IN the value is left in AL/AX/EAX.
;
; The pass-through must not use a plain IN/OUT: the port is still trapped,
; so from V86 that would fault straight back in here.  Bytes go through
; QPI 1A00h/1A01h (one exact byte cycle at ring 0).  Words, dwords and
; string elements untrap the port(s), run the guest's exact instruction,
; and retrap - QPI 1A04h would split a word into two byte cycles, and this
; card's 16-bit window is exactly where that difference could matter.
trap_handler:
        push bx
        mov  bx, dx
        sub  bx, [cs:w0]
        cmp  bx, 16
        jb   .ours
        mov  bx, dx
        sub  bx, [cs:w1]
        cmp  bx, 8
        jb   .ours1
        pop  bx
        cmp  word [cs:oldtrap+2], 0
        je   .absorb
        add  dword [cs:nchain], 1
        jmp  far [cs:oldtrap]
.absorb:
        retf
.ours1:
        add  bx, 16
.ours:
        pushf                         ; older QPIEMU may call us with IF set
        cli
        mov  [cs:tmpidx], bx
        test cl, 38h
        jnz  near .wide
        test cl, 4
        jnz  .bout
        ; ---- byte IN: QPI 1A00h, value in BL
        push ax
        push cx
        push dx
        mov  ax, 1A00h
        call far [cs:qpi]
        pop  dx
        pop  cx
        pop  ax
        mov  al, bl
        mov  [cs:tmpval], al
        mov  byte [cs:tmpval+1], 0
        mov  word [cs:tmpkind], 0
        jmp  .log1
        ; ---- byte OUT: QPI 1A01h, value in BL
.bout:
        push ax
        push cx
        push dx
        mov  bl, al
        mov  ax, 1A01h
        call far [cs:qpi]
        pop  dx
        pop  cx
        pop  ax
        mov  [cs:tmpval], al
        mov  byte [cs:tmpval+1], 0
        mov  word [cs:tmpkind], 2
        jmp  .log1

        ; ---- word / dword / string: untrap, exact access, retrap
.wide:
        mov  word [cs:tmpn], 1
        test cl, 8
        jz   .sz1
        mov  word [cs:tmpn], 2
.sz1:
        test cl, 10h
        jz   .sz2
        mov  word [cs:tmpn], 4
.sz2:
        mov  word [cs:tmpfn], 1A0Ah
        call qpi_range
        test cl, 20h
        jnz  near .str
        test cl, 4
        jnz  .wout
        test cl, 10h
        jnz  .wind
        in   ax, dx
        mov  [cs:tmpval], ax
        mov  word [cs:tmpkind], 1
        jmp  .retrap1
.wind:
        in   eax, dx
        jmp  .retrapd
.wout:
        test cl, 10h
        jnz  .woutd
        out  dx, ax
        mov  [cs:tmpval], ax
        mov  word [cs:tmpkind], 3
        jmp  .retrap1
.woutd:
        out  dx, eax
        ; ---- dword: two entries, low word at port, high word at port+2
.retrapd:
        mov  word [cs:tmpfn], 1A09h
        call qpi_range
        mov  word [cs:tmpkind], 4
        mov  [cs:tmpval], ax
        call do_log
        push eax
        push dx
        shr  eax, 16
        mov  [cs:tmpval], ax
        add  dx, 2
        call do_log
        pop  dx
        pop  eax
        jmp  .exit
.retrap1:
        mov  word [cs:tmpfn], 1A09h
        call qpi_range
.log1:
        call do_log
.exit:
        popf
        pop  bx
        retf

        ; ---- string: ONE element from/to the guest's DS:SI / ES:DI.
        ;      AX is the guest's and must survive (Jemm does not store
        ;      EAX back for string I/O).  SI/DI advance per the guest DF.
.str:
        test cl, 40h
        jz   .snorep
        add  dword [cs:nrep], 1
.snorep:
        push eax
        test cl, 4
        jnz  .sout
        test cl, 10h
        jnz  .sind
        test cl, 8
        jnz  .sinw
        in   al, dx
        stosb
        xor  ah, ah
        jmp  .sdone
.sinw:
        in   ax, dx
        stosw
        jmp  .sdone
.sind:
        in   eax, dx
        stosd
        jmp  .sdone
.sout:
        test cl, 10h
        jnz  .soutd
        test cl, 8
        jnz  .soutw
        lodsb
        out  dx, al
        xor  ah, ah
        jmp  .sdone
.soutw:
        lodsw
        out  dx, ax
        jmp  .sdone
.soutd:
        lodsd
        out  dx, eax
.sdone:
        mov  word [cs:tmpfn], 1A09h
        call qpi_range
        mov  word [cs:tmpkind], 4
        mov  [cs:tmpval], ax          ; low word of the element
        call do_log
        pop  eax
        jmp  .exit

; ---- qpi_range: QPI [tmpfn] (1A09h trap / 1A0Ah untrap) on each port in
;      DX .. DX+[tmpn]-1 that is ours.  Preserves every register. --------
qpi_range:
        pushad
        push es
        mov  cx, [cs:tmpn]
.q:
        mov  bx, dx
        sub  bx, [cs:w0]
        cmp  bx, 16
        jb   .mine
        mov  bx, dx
        sub  bx, [cs:w1]
        cmp  bx, 8
        jae  .skip
.mine:
        push cx
        push dx
        mov  ax, [cs:tmpfn]
        call far [cs:qpi]
        pop  dx
        pop  cx
        jnc  .skip
        add  dword [cs:nfail], 1
.skip:
        inc  dx
        loop .q
        pop  es
        popad
        ret

; ---- do_log: DX port, CL type, [tmpidx] [tmpval] [tmpkind] ---------------
do_log:
        pushad
        push es
        add  dword [cs:nevents], 1
        movzx esi, word [cs:tmpidx]
        lea  esi, [esi*4+esi]
        movzx eax, word [cs:tmpkind]
        add  esi, eax
        add  dword [cs:counts+esi*4], 1

        cmp  byte [cs:armed], 0
        je   near .done
        movzx bp, byte [cs:nfilt]
        xor  si, si
.f:
        or   bp, bp
        jz   .nf
        cmp  dx, [cs:filt+si]
        je   near .done
        add  si, 2
        dec  bp
        jmp  .f
.nf:
        mov  ax, [cs:tmpval]
        ; collapse an exact repeat of the previous entry
        cmp  dword [cs:nlog], 0
        je   .new
        les  di, [cs:lastp]
        cmp  [es:di], dx
        jne  .new
        cmp  [es:di+2], ax
        jne  .new
        cmp  [es:di+4], cl
        jne  .new
        cmp  byte [es:di+5], 0FFh
        je   .new
        inc  byte [es:di+5]
        jmp  .done
.new:
        mov  ebx, [cs:nlog]
        cmp  ebx, [cs:bufcap]
        jb   .room
        add  dword [cs:ndrop], 1
        jmp  .done
.room:
        les  di, [cs:wrptr]
        mov  [cs:lastp], di
        mov  [cs:lastp+2], es
        mov  [es:di], dx
        mov  [es:di+2], ax
        mov  [es:di+4], cl
        mov  byte [es:di+5], 0
        push ds
        xor  ax, ax
        mov  ds, ax
        mov  ax, [046Ch]              ; BIOS tick, low word
        pop  ds
        mov  [es:di+6], ax
        mov  al, 0C2h                 ; 8254 read-back: latch count and
        out  43h, al                  ; status of channel 0
        in   al, 40h                  ; status first
        mov  [es:di+10], al
        mov  byte [es:di+11], 0
        in   al, 40h
        mov  ah, al
        in   al, 40h
        xchg al, ah
        mov  [es:di+8], ax
        add  di, ENTSZ                ; normalise so the offset never wraps
        mov  ax, di
        shr  ax, 4
        mov  bx, es
        add  bx, ax
        and  di, 0Fh
        mov  [cs:wrptr], di
        mov  [cs:wrptr+2], bx
        add  dword [cs:nlog], 1
.done:
        pop  es
        popad
        ret

resident_end:

;==========================================================================
;  TRANSIENT
;==========================================================================
qpient    dd 0
jent      dd 0                        ; IBMTRJ V86 entry, seg 0 = not loaded
usejlm    db 0
f_q       db 0
f_d       db 0
o_strmax  dw 0
res       dw 0                        ; resident copy's segment, 0 = none
o_k       dw 128
o_io1     dw 0250h
o_io2     dw 0FFFFh
f_c       db 0
f_stop    db 0
f_s       db 0
f_u       db 0
f_w       db 0
f_help    db 0
o_nfilt   db 0
o_filt    dw 0, 0, 0, 0
fname     times 80 db 0

init:
        cld
        mov  sp, stk_top              ; own stack inside the image, so the
                                      ; block can shrink before the log is
                                      ; allocated
        mov  si, m_banner
        call prstr
        call parse_args
        cmp  byte [f_help], 0
        je   .nh
        mov  si, m_help
        call prstr
        int  20h
.nh:
        cmp  byte [f_q], 0
        jne  .qpi
        call find_jlm
        cmp  word [jent+2], 0
        je   .qpi
        mov  byte [usejlm], 1
        mov  ax, 0                    ; IBMTRJ status: BX = armed copy
        call far [jent]
        or   bx, bx
        jz   .nores
        mov  es, bx
        jmp  .chk
.qpi:
        call find_qpi
        cmp  word [qpient+2], 0
        jne  .hq
        mov  si, m_noqpi
        jmp  errout
.hq:
        ; a resident IBMTRACE holds the QPI slot?
        mov  ax, 1A06h
        call far [qpient]
        jc   near qpifail
        mov  ax, es
        or   ax, ax
        jz   .nores
.chk:
        cmp  word [es:103h], 'IB'
        jne  .nores
        cmp  word [es:105h], 'TR'
        jne  .nores
        mov  [res], es
.nores:
        mov  al, [f_c]
        or   al, [f_stop]
        or   al, [f_s]
        or   al, [f_u]
        or   al, [f_w]
        jz   install
        cmp  word [res], 0
        jne  .have
        mov  si, m_notres
        jmp  errout
.have:
        cmp  byte [f_u], 0
        jne  near do_unload
        cmp  byte [f_stop], 0
        je   .ns
        mov  es, [res]
        mov  byte [es:armed], 0
        mov  si, m_stopped
        call prstr
.ns:
        cmp  byte [f_c], 0
        je   .nc
        call do_clear
.nc:
        cmp  byte [f_s], 0
        je   .nsum
        call do_summary
.nsum:
        cmp  byte [f_w], 0
        je   .nw
        call do_write
.nw:
        int  20h

; ---- install ------------------------------------------------------------
install:
        cmp  word [res], 0
        je   .fresh
        mov  si, m_already
        jmp  errout
.fresh:
        mov  ax, [o_io2]
        cmp  ax, 0FFFFh
        jne  .i2
        mov  ax, [o_io1]              ; pair window 1 with the alias
        and  ax, 0FC00h
        or   ax, 0340h
        mov  [o_io2], ax
.i2:
        mov  ax, [o_io1]
        mov  [w0], ax
        mov  ax, [o_io2]
        mov  [w1], ax
        mov  ax, [w0]                 ; never trap the PCIC (3E0-3E7) or the
        call unsafe                   ; COMrade link (3F8-3FF)
        jc   .bad
        test ax, 0Fh
        jnz  .bad
        mov  ax, [w1]
        call unsafe
        jc   .bad
        test ax, 07h
        jz   .okio
.bad:
        mov  si, m_badio
        jmp  errout
.okio:
        call copy_filt_local
        ; log size -> paragraphs, entries
        mov  ax, [o_k]
        cmp  ax, 16
        jae  .k1
        mov  ax, 16
.k1:
        cmp  ax, 320
        jbe  .k2
        mov  ax, 320
.k2:
        mov  [o_k], ax
        ; shrink to the whole image (transient code and stack are still
        ; running), allocate the log, and only at 31h drop to resident size
        mov  bx, (stk_top - $$ + 100h + 15) >> 4
        mov  ah, 4Ah
        push cs
        pop  es
        int  21h
        jc   near nomem
        mov  bx, [o_k]
        shl  bx, 6                    ; KB -> paragraphs
        mov  ah, 48h
        int  21h
        jc   near nomem
        mov  [bufseg0], ax
        mov  [wrptr+2], ax
        mov  word [wrptr], 0
        movzx eax, word [o_k]
        shl  eax, 10
        xor  edx, edx
        mov  ecx, ENTSZ
        div  ecx
        mov  [bufcap], eax

        mov  ax, [o_strmax]
        mov  [strmax], ax
        cmp  byte [usejlm], 0
        je   .viaqpi
        mov  byte [backend], 1
        mov  byte [armed], 1
        push cs
        pop  es
        mov  ax, 1                    ; IBMTRJ: arm on our header
        call far [jent]
        jnc  .report
        push ax
        mov  es, [bufseg0]
        mov  ah, 49h
        int  21h
        mov  si, m_jfail
        call prstr
        pop  ax
        call prhex3
        mov  si, m_crlf
        jmp  errout
.viaqpi:
        mov  eax, [qpient]            ; the handler's pass-through uses it
        mov  [qpi], eax
        ; current handler becomes our chain
        mov  ax, 1A06h
        call far [qpient]
        jc   near qpifail
        mov  [oldtrap], di
        mov  [oldtrap+2], es
        push cs
        pop  es
        mov  di, trap_handler
        mov  ax, 1A07h
        call far [qpient]
        jc   near qpifail
        ; trap the 24 ports; a refusal undoes what we did
        xor  bx, bx
.tp:
        call port_of_bx
        mov  ax, 1A09h
        push bx
        call far [qpient]
        pop  bx
        jc   .undo
        inc  bx
        cmp  bx, NPORT
        jb   .tp
        mov  byte [armed], 1
.report:
        mov  si, m_ok1
        call prstr
        mov  ax, [w0]
        call prhex3
        mov  al, '-'
        call prchar
        mov  ax, [w0]
        add  ax, 0Fh
        call prhex3
        mov  si, m_ok2
        call prstr
        mov  ax, [w1]
        call prhex3
        mov  al, '-'
        call prchar
        mov  ax, [w1]
        add  ax, 07h
        call prhex3
        mov  si, m_ok3
        call prstr
        mov  eax, [bufcap]
        call prdec
        mov  si, m_ok4
        call prstr
        mov  si, m_bq
        cmp  byte [backend], 0
        je   .bk
        mov  si, m_bj
.bk:
        call prstr
        ; free the environment, go resident
        mov  es, [2Ch]
        mov  ah, 49h
        int  21h
        mov  word [2Ch], 0
        mov  dx, (resident_end - $$ + 100h + 15) >> 4
        mov  ax, 3100h
        int  21h
.undo:
        or   bx, bx
        jz   .undoh
        dec  bx
        call port_of_bx
        mov  ax, 1A0Ah
        push bx
        call far [qpient]
        pop  bx
        jmp  .undo
.undoh:
        les  di, [oldtrap]
        mov  ax, 1A07h
        call far [qpient]
        mov  si, m_taken
        jmp  errout

; BX = index 0..23 -> DX = port, using the header in DS
port_of_bx:
        mov  dx, bx
        cmp  bx, 16
        jae  .p1
        add  dx, [w0]
        ret
.p1:
        sub  dx, 16
        add  dx, [w1]
        ret

; AX = block base; CF = the block reaches 3E0h-3FFh
unsafe:
        cmp  ax, 03D0h
        jb   .ok
        cmp  ax, 0400h
        jae  .ok
        stc
        ret
.ok:
        clc
        ret

copy_filt_local:
        mov  al, [o_nfilt]
        mov  [nfilt], al
        mov  ax, [o_filt]
        mov  [filt], ax
        mov  ax, [o_filt+2]
        mov  [filt+2], ax
        mov  ax, [o_filt+4]
        mov  [filt+4], ax
        mov  ax, [o_filt+6]
        mov  [filt+6], ax
        ret

; ---- /C: clear and re-arm the resident copy -----------------------------
do_clear:
        mov  es, [res]
        cli
        mov  byte [es:armed], 0
        mov  di, nlog
        mov  cx, (lastp - nlog) / 2
        xor  ax, ax
        rep  stosw
        mov  word [es:lastp], 0
        mov  word [es:lastp+2], 0
        mov  dword [es:nfail], 0
        mov  ax, [es:bufseg0]
        mov  [es:wrptr+2], ax
        mov  word [es:wrptr], 0
        cmp  byte [o_nfilt], 0        ; new filters only if given
        je   .kf
        mov  al, [o_nfilt]
        mov  [es:nfilt], al
        mov  si, o_filt
        mov  di, filt
        mov  cx, 4
        rep  movsw
.kf:
        cmp  byte [f_d], 0
        je   .kd
        mov  ax, [o_strmax]
        mov  [es:strmax], ax
.kd:
        mov  byte [es:armed], 1
        sti
        mov  si, m_cleared
        call prstr
        ret

; ---- /S: summary ---------------------------------------------------------
do_summary:
        mov  es, [res]
        mov  si, m_s_ev
        call prstr
        mov  eax, [es:nevents]
        call prdec
        mov  si, m_s_log
        call prstr
        mov  eax, [es:nlog]
        call prdec
        mov  al, '/'
        call prchar
        mov  eax, [es:bufcap]
        call prdec
        mov  si, m_s_drop
        call prstr
        mov  eax, [es:ndrop]
        call prdec
        mov  si, m_s_rep
        cmp  byte [es:backend], 0
        je   .rl
        mov  si, m_s_repj
.rl:
        call prstr
        mov  eax, [es:nrep]
        call prdec
        mov  si, m_s_chain
        call prstr
        mov  eax, [es:nchain]
        call prdec
        mov  si, m_s_fail
        call prstr
        mov  eax, [es:nfail]
        call prdec
        mov  si, m_s_arm
        call prstr
        mov  al, [es:armed]
        add  al, '0'
        call prchar
        mov  si, m_s_hdr
        call prstr
        xor  bx, bx
.p:
        ; any nonzero count for this port?
        mov  si, bx
        imul si, si, NKIND*4
        add  si, counts
        mov  cx, NKIND
        xor  eax, eax
.any:
        or   eax, [es:si]
        add  si, 4
        loop .any
        or   eax, eax
        jz   .next
        mov  dx, bx
        cmp  bx, 16
        jae  .w1
        add  dx, [es:w0]
        jmp  .pp
.w1:
        sub  dx, 16
        add  dx, [es:w1]
.pp:
        mov  ax, dx
        call prhex3
        mov  si, bx
        imul si, si, NKIND*4
        add  si, counts
        mov  cx, NKIND
.c:
        push si
        push cx
        mov  eax, [es:si]
        call prdec_col
        pop  cx
        pop  si
        add  si, 4
        loop .c
        mov  si, m_crlf
        call prstr
.next:
        inc  bx
        cmp  bx, NPORT
        jb   .p
        ret

; ---- /W=file: stop logging, write header + entries ----------------------
do_write:
        mov  es, [res]
        mov  byte [es:armed], 0
        mov  dx, fname
        xor  cx, cx
        mov  ah, 3Ch
        int  21h
        jc   near .ferr
        mov  bx, ax
        push ds
        mov  ds, [res]
        mov  dx, 103h
        mov  cx, hdr_end - 103h
        mov  ah, 40h
        int  21h
        pop  ds
        jc   .werr
        mov  es, [res]
        mov  eax, [es:nlog]
        mov  ecx, ENTSZ
        mul  ecx
        mov  esi, eax                 ; bytes left
        mov  bp, [es:bufseg0]
.chunk:
        or   esi, esi
        jz   .closed
        mov  ecx, 8000h
        cmp  esi, ecx
        jae  .full
        mov  ecx, esi
.full:
        push ds
        mov  ds, bp
        xor  dx, dx
        mov  ah, 40h
        int  21h
        pop  ds
        jc   .werr
        movzx ecx, cx
        sub  esi, ecx
        add  bp, 800h
        jmp  .chunk
.closed:
        mov  ah, 3Eh
        int  21h
        mov  si, m_wrote
        call prstr
        mov  es, [res]
        mov  eax, [es:nlog]
        call prdec
        mov  si, m_wrote2
        call prstr
        ret
.werr:
        mov  ah, 3Eh
        int  21h
.ferr:
        mov  si, m_ferr
        jmp  errout

; ---- /U -----------------------------------------------------------------
do_unload:
        mov  es, [res]
        mov  ax, [es:w0]
        mov  [w0], ax
        mov  ax, [es:w1]
        mov  [w1], ax
        mov  byte [es:armed], 0
        cmp  byte [es:backend], 0
        je   .viaqpi
        mov  ax, 2                    ; IBMTRJ: disarm
        call far [jent]
        jmp  .free
.viaqpi:
        xor  bx, bx
.u:
        call port_of_bx
        mov  ax, 1A0Ah
        push bx
        call far [qpient]
        pop  bx
        inc  bx
        cmp  bx, NPORT
        jb   .u
        mov  es, [res]
        mov  di, [es:oldtrap]
        mov  ax, [es:oldtrap+2]
        mov  es, ax
        mov  ax, 1A07h
        call far [qpient]
.free:
        mov  es, [res]
        mov  ax, [es:bufseg0]
        mov  es, ax
        mov  ah, 49h
        int  21h
        mov  es, [res]
        mov  ah, 49h
        int  21h
        mov  si, m_unloaded
        call prstr
        int  20h

; ---- find_qpi: int 67h 3F00h 'QEMM', else QPIEMU int 2Fh 1684h/4354h ----
find_qpi:
        push es
        xor  ax, ax
        mov  es, ax
        mov  ax, [es:67h*4]
        or   ax, [es:67h*4+2]
        pop  es
        jz   .t2
        mov  ax, 3F00h
        mov  cx, 5145h
        mov  dx, 4D4Dh
        push es
        int  67h
        mov  bx, es
        pop  es
        or   ah, ah
        jnz  .t2
        or   bx, bx
        jz   .t2
        mov  [qpient], di
        mov  [qpient+2], bx
        ret
.t2:
        mov  ax, 1684h
        mov  bx, 4354h
        push es
        xor  di, di
        mov  es, di
        int  2Fh
        mov  bx, es
        pop  es
        or   al, al
        jnz  .no
        or   bx, bx
        jz   .no
        mov  [qpient], di
        mov  [qpient+2], bx
.no:
        ret

; ---- find_jlm: IBMTRJ's V86 entry, INT 2Fh AX=1684h BX=4954h ------------
find_jlm:
        mov  ax, 1684h
        mov  bx, 4954h
        push es
        xor  di, di
        mov  es, di
        int  2Fh
        mov  bx, es
        pop  es
        or   al, al
        jnz  .no
        or   bx, bx
        jz   .no
        mov  [jent], di
        mov  [jent+2], bx
.no:
        ret

; ---- argument parsing ----------------------------------------------------
parse_args:
        mov  si, 81h
.next:
        lodsb
        cmp  al, 13
        je   .end
        cmp  al, '/'
        jne  .next
        lodsb
        call upcase
        cmp  al, '?'
        je   .help
        cmp  al, 'I'
        je   .io
        cmp  al, 'K'
        je   .k
        cmp  al, 'N'
        je   .n
        cmp  al, 'C'
        je   .c
        cmp  al, 'S'
        je   .s
        cmp  al, 'W'
        je   .w
        cmp  al, 'U'
        je   .u
        cmp  al, 'D'
        je   .d
        cmp  al, 'Q'
        je   .q
        jmp  .next
.end:
        ret
.help:
        mov  byte [f_help], 1
        jmp  .next
.u:
        mov  byte [f_u], 1
        jmp  .next
.q:
        mov  byte [f_q], 1
        jmp  .next
.d:
        call eat_eq
        call decnum
        mov  [o_strmax], dx
        mov  byte [f_d], 1
        jmp  .next
.c:
        mov  byte [f_c], 1
        jmp  .next
.s:
        lodsb                         ; /S or /STOP
        call upcase
        cmp  al, 'T'
        je   .stop
        dec  si
        mov  byte [f_s], 1
        jmp  .next
.stop:
        mov  byte [f_stop], 1
        jmp  .next
.io:
        lodsb                         ; O
        lodsb                         ; 1 or 2
        mov  bl, al
        call eat_eq
        call hexnum
        cmp  bl, '2'
        je   .io2
        mov  [o_io1], dx
        jmp  .next
.io2:
        mov  [o_io2], dx
        jmp  .next
.k:
        call eat_eq
        call decnum
        mov  [o_k], dx
        jmp  .next
.n:
        call eat_eq
        call hexnum
        movzx bx, byte [o_nfilt]
        cmp  bx, 4
        jae  .next
        shl  bx, 1
        mov  [o_filt+bx], dx
        inc  byte [o_nfilt]
        jmp  .next
.w:
        call eat_eq
        mov  di, fname
        mov  cx, 79
.wc:
        lodsb
        cmp  al, ' '
        jbe  .wd
        stosb
        loop .wc
        inc  si
.wd:
        dec  si
        mov  byte [di], 0
        cmp  di, fname
        je   .next
        mov  byte [f_w], 1
        jmp  .next

eat_eq:                               ; skip to past '=' (or stop at space/CR)
        lodsb
        cmp  al, '='
        je   .r
        cmp  al, ':'
        je   .r
        cmp  al, ' '
        jbe  .back
        jmp  eat_eq
.back:
        dec  si
.r:
        ret

hexnum:                               ; DS:SI hex -> DX
        xor  dx, dx
.l:
        lodsb
        call upcase
        cmp  al, '0'
        jb   .e
        cmp  al, '9'
        jbe  .d
        cmp  al, 'A'
        jb   .e
        cmp  al, 'F'
        ja   .e
        sub  al, 7
.d:
        sub  al, '0'
        shl  dx, 4
        or   dl, al
        jmp  .l
.e:
        dec  si
        ret

decnum:                               ; DS:SI decimal -> DX
        xor  dx, dx
.l:
        lodsb
        cmp  al, '0'
        jb   .e
        cmp  al, '9'
        ja   .e
        sub  al, '0'
        movzx ax, al
        imul dx, dx, 10
        add  dx, ax
        jmp  .l
.e:
        dec  si
        ret

upcase:
        cmp  al, 'a'
        jb   .r
        cmp  al, 'z'
        ja   .r
        sub  al, 20h
.r:
        ret

; ---- output --------------------------------------------------------------
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
prhex3:
        push ax
        mov  al, ah
        call prnyb
        pop  ax
        push ax
        shr  al, 4
        call prnyb
        pop  ax
        push ax
        call prnyb
        pop  ax
        ret
prnyb:
        and  al, 0Fh
        add  al, '0'
        cmp  al, '9'
        jbe  .o
        add  al, 7
.o:
        jmp  prchar
prdec:                                ; EAX unsigned decimal
        push eax
        push ebx
        push ecx
        push edx
        mov  ebx, 10
        xor  ecx, ecx
.d:
        xor  edx, edx
        div  ebx
        push dx
        inc  cx
        or   eax, eax
        jnz  .d
.p:
        pop  ax
        add  al, '0'
        call prchar
        loop .p
        pop  edx
        pop  ecx
        pop  ebx
        pop  eax
        ret
prdec_col:                            ; EAX right-aligned in 11 columns
        push eax
        push ecx
        push edx
        push ebx
        mov  ebx, 10
        xor  ecx, ecx
        push eax
.cnt:
        xor  edx, edx
        div  ebx
        inc  cx
        or   eax, eax
        jnz  .cnt
        pop  eax
        mov  dx, 11
        sub  dx, cx
.sp:
        mov  al, ' '
        push eax
        call prchar
        pop  eax
        dec  dx
        jnz  .sp
        pop  ebx
        pop  edx
        pop  ecx
        pop  eax
        jmp  prdec

nomem:
        mov  si, m_nomem
        jmp  errout
qpifail:
        mov  si, m_qpifail
errout:
        call prstr
        mov  ax, 4C01h
        int  21h

m_banner:  db 'IBMTRACE 0.3 - I/O trace of the IBM PCMCIA Audio Adapter',13,10,0
m_help:    db 'IBMTRACE [/IO1=250] [/IO2=340] [/K=128] [/N=port] [/D=n] [/Q]  install',13,10
           db 'IBMTRACE /C [/N=port]   clear + re-arm      /STOP  stop logging',13,10
           db 'IBMTRACE /S             summary             /W=file  write log',13,10
           db 'IBMTRACE /U             unload',13,10
           db 'Backend: JLOAD IBMTRJ.DLL (ring 0), else a QPI host (QPIEMU / QEMM).',13,10,0
m_noqpi:   db 'No QPI host (load JLOAD QPIEMU.DLL under JEMM, or QEMM).',13,10,0
m_notres:  db 'IBMTRACE is not resident (or another client holds the QPI slot).',13,10,0
m_already: db 'IBMTRACE is already resident.  /U first.',13,10,0
m_nomem:   db 'Not enough DOS memory for the log.  Try a smaller /K.',13,10,0
m_qpifail: db 'QPI call failed.',13,10,0
m_taken:   db 'QPI refused a port trap (already trapped?).  Nothing left installed.',13,10,0
m_ok1:     db 'Trapping ',0
m_ok2:     db ' and ',0
m_ok3:     db ', log ',0
m_ok4:     db ' entries, armed',0
m_bq:      db ' (QPI trap).',13,10,0
m_bj:      db ' (ring 0, IBMTRJ).',13,10,0
m_jfail:   db 'IBMTRJ refused to arm, code ',0
m_stopped: db 'Logging stopped.',13,10,0
m_cleared: db 'Log and counters cleared, armed.',13,10,0
m_s_ev:    db 'events ',0
m_s_log:   db '  logged ',0
m_s_drop:  db '  dropped ',0
m_s_rep:   db 13,10,'rep lost ',0
m_s_repj:  db 13,10,'rep ops ',0
m_s_chain: db '  chained ',0
m_s_fail:  db '  qpi fail ',0
m_s_arm:   db '  armed ',0
m_s_hdr:   db 13,10,'port       in b       in w      out b      out w      other',13,10,0
m_crlf:    db 13,10,0
m_wrote:   db 'Wrote ',0
m_wrote2:  db ' entries.  Logging stopped; /C re-arms.',13,10,0
m_badio:   db 'Bad /IO1 or /IO2 (alignment, or reaches 3E0-3FF).',13,10,0
m_ferr:    db 'File write failed.',13,10,0
m_unloaded: db 'IBMTRACE unloaded.',13,10,0

        align 2
stk:       times 512 db 0
stk_top:
