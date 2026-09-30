;==========================================================================
;  IBMTRJ - ring-0 port trap for IBMTRACE, as a JLM (Jemm Loadable Module).
;
;    JLOAD IBMTRJ.DLL        then IBMTRACE finds it and uses it
;    JLOAD -u IBMTRJ.DLL     unload (disarms first)
;
;  Why a JLM: a QPI (V86) trap handler never sees the guest's REP count -
;  QPIEMU overwrites the guest CX with the access type before calling it -
;  and the IBM adapter's driver feeds its FIFO with REP OUTSW.  At ring 0
;  the guest registers are intact, so this handler performs the whole
;  string operation exactly (count, DS:SI / ES:DI, direction flag), writes
;  SI/DI and CX back, and logs the count and the data.  Single accesses are
;  performed with the guest's exact width.  Ring-0 I/O ignores the I/O
;  permission bitmap, so there is no re-trap.
;
;  V86 API (entry from INT 2Fh AX=1684h BX=4954h -> ES:DI, AL=0):
;    AX=0  status: BX = segment of the armed IBMTRACE copy (0 = none)
;    AX=1  arm: ES = segment whose :0103h holds the IBMTRACE header;
;          traps the 24 ports named in it.  CF + AX: 1 busy, 2 bad header,
;          3 a port could not be trapped (nothing left trapped).
;    AX=2  disarm: untrap everything.
;
;  The log format is IBMTRACE's (12-byte entries in DOS memory).  A string
;  operation logs one entry (type = access type, value = element count)
;  followed by its elements (type 80h + the size/direction bits), up to the
;  header's strmax (0 = all).
;
;  Build (Open Watcom's JWasm and wlink; JWasm's -pe mode crashed on the host):
;    jwasm -c -coff -nologo -I<dir of JLM.INC> -FoIBMTRJ.OBJ IBMTRJ.ASM
;    wlink @IBMTRJ.LNK
;    python3 pe2px.py IBMTRJ.DLL      (checks it, then PE -> PX for JLOAD)
;  JLM.INC is Jemm's (public domain, github.com/Baron-von-Riedesel/Jemm).
;
;  (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
;  Original work.  The JLM module shape (DDB, V86 API, DllMain) follows
;  Jemm's public-domain QPIEMU/IOTRAP samples.
;==========================================================================
        .386
        .model flat, stdcall

        .nolist
        include jlm.inc
        .list

DEVICE_ID   equ 4954h
NPORT       equ 24
NKIND       equ 5
ENTSZ       equ 12

DLL_PROCESS_ATTACH  equ 1
DLL_PROCESS_DETACH  equ 0

;--- IBMTRACE header fields, as offsets from 'IBTR' (PSP:0103h)
H_ARMED     equ 005h
H_NFILT     equ 006h
H_W0        equ 007h
H_W1        equ 009h
H_FILT      equ 00Bh
H_BUFSEG    equ 017h
H_BUFCAP    equ 019h
H_NLOG      equ 021h
H_NEV       equ 025h
H_NDROP     equ 029h
H_NREP      equ 02Dh
H_COUNTS    equ 035h
H_STRMAX    equ 21Dh

        .data

ifdef ?PE
        public export ddb
        option dotname
.drectve segment info
        db "-subsystem:native -dll -fixed:no"
.drectve ends
.hdr$2 segment flat
        db "PX"
.hdr$2 ends
else
        public ddb
endif

ddb VxD_Desc_Block <0,0,DEVICE_ID,1,0,0,"IBMTRJ",0,0, v86_dispatch >

hdr     dd 0                    ; linear address of 'IBTR'; 0 = disarmed
hseg    dd 0                    ; its segment
ports   dw NPORT dup (0)
s_base  dd 0                    ; string op: segment base (linear)
s_off   dd 0                    ;            current SI / DI
s_step  dd 0                    ;            +-1/2/4
s_left  dd 0                    ;            elements still to move
s_nlog  dd 0                    ;            elements logged so far

        .code

;--- V86 API dispatcher -----------------------------------------------------
v86_dispatch proc
        @VMMCall Simulate_Far_Ret
        and [ebp].Client_Reg_Struc.Client_EFlags, not 1
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EAX
        cmp eax, 0
        jz fn_status
        cmp eax, 1
        jz fn_arm
        cmp eax, 2
        jz fn_disarm
fail:
        or [ebp].Client_Reg_Struc.Client_EFlags, 1
        ret
fn_status:
        mov eax, hseg
        mov word ptr [ebp].Client_Reg_Struc.Client_EBX, ax
        mov word ptr [ebp].Client_Reg_Struc.Client_EAX, 0100h
        ret
fn_arm:
        call arm
        jnc @F
        mov word ptr [ebp].Client_Reg_Struc.Client_EAX, ax
        jmp fail
@@:
        ret
fn_disarm:
        call disarm
        ret
v86_dispatch endp

;--- arm: CF + AX = error ---------------------------------------------------
arm proc uses ebx esi edi
        cmp hdr, 0
        jnz busy
        movzx ecx, word ptr [ebp].Client_Reg_Struc.Client_ES
        mov ebx, ecx
        shl ebx, 4
        add ebx, 103h
        cmp dword ptr [ebx], 52544249h          ; 'IBTR'
        jnz badhdr
        movzx eax, word ptr [ebx+H_W0]
        movzx edx, word ptr [ebx+H_W1]
        xor edi, edi
nx0:
        lea esi, [eax+edi]
        mov ports[edi*2], si
        inc edi
        cmp edi, 16
        jb nx0
nx1:
        lea esi, [edx+edi-16]
        mov ports[edi*2], si
        inc edi
        cmp edi, NPORT
        jb nx1
        mov hdr, ebx
        mov hseg, ecx
        xor edi, edi
inst:
        movzx edx, ports[edi*2]
        mov esi, offset iocb
        @VMMCall Install_IO_Handler
        jc undo
        inc edi
        cmp edi, NPORT
        jb inst
        clc
        ret
undo:
        or edi, edi
        jz undone
        dec edi
        movzx edx, ports[edi*2]
        @VMMCall Remove_IO_Handler
        jmp undo
undone:
        mov hdr, 0
        mov hseg, 0
        mov ax, 3
        stc
        ret
busy:
        mov ax, 1
        stc
        ret
badhdr:
        mov ax, 2
        stc
        ret
arm endp

;--- disarm ---------------------------------------------------------------
disarm proc uses edi
        cmp hdr, 0
        jz done
        xor edi, edi
rm:
        movzx edx, ports[edi*2]
        @VMMCall Remove_IO_Handler
        inc edi
        cmp edi, NPORT
        jb rm
        mov hdr, 0
        mov hseg, 0
done:
        ret
disarm endp

;--- the I/O handler --------------------------------------------------------
;--- in:  ECX = access type (bit 2 OUT, 3 word, 4 dword, 5 string, 6 REP;
;---             for string I/O the high word is the segment),
;---       EDX = port, EBP = client registers, EAX = client EAX
;--- out: EAX (stored into the client EAX for a non-string IN)
iocb proc
        push ebx
        push esi
        push edi
        mov ebx, hdr
        test cl, STRING_IO
        jnz strio
        test cl, OUTPUT
        jnz isout
        test cl, DWORD_IO
        jnz ind
        test cl, WORD_IO
        jnz inw
        in al, dx
        movzx esi, al
        xor edi, edi
        jmp one
inw:
        in ax, dx
        movzx esi, ax
        mov edi, 1
        jmp one
ind:
        in eax, dx
        jmp dwd
isout:
        test cl, DWORD_IO
        jnz outd
        test cl, WORD_IO
        jnz outw
        out dx, al
        movzx esi, al
        mov edi, 2
        jmp one
outw:
        out dx, ax
        movzx esi, ax
        mov edi, 3
        jmp one
outd:
        out dx, eax
dwd:                                    ; low word at port, high at port+2
        push eax
        movzx esi, ax
        mov edi, 4
        call count_log
        mov eax, [esp]
        shr eax, 16
        movzx esi, ax
        push edx
        add edx, 2
        mov edi, -1
        call count_log
        pop edx
        pop eax
        jmp iexit
one:
        push eax
        call count_log
        pop eax
iexit:
        pop edi
        pop esi
        pop ebx
        ret

;--- string I/O: the whole operation, exactly -------------------------------
strio:
        mov edi, 1
        test cl, REP_IO
        jz @F
        movzx edi, word ptr [ebp].Client_Reg_Struc.Client_ECX
@@:
        mov s_left, edi
        mov esi, edi                    ; header entry: value = count
        mov edi, 4
        call count_log
        or ebx, ebx
        jz @F
        test cl, REP_IO
        jz @F
        inc dword ptr [ebx+H_NREP]
@@:
        mov eax, ecx                    ; segment base
        shr eax, 16
        shl eax, 4
        mov s_base, eax
        mov eax, 1
        test cl, WORD_IO
        jz @F
        mov eax, 2
@@:
        test cl, DWORD_IO
        jz @F
        mov eax, 4
@@:
        test byte ptr [ebp].Client_Reg_Struc.Client_EFlags+1, 4     ; DF
        jz @F
        neg eax
@@:
        mov s_step, eax
        test cl, OUTPUT
        jz @F
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_ESI
        jmp sgo
@@:
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EDI
sgo:
        mov s_off, eax
        mov s_nlog, 0
elem:
        cmp s_left, 0
        jz sdone
        mov esi, s_off
        and esi, 0FFFFh
        add esi, s_base
        test cl, OUTPUT
        jz ein
        test cl, DWORD_IO
        jnz eod
        test cl, WORD_IO
        jnz eow
        mov al, [esi]
        out dx, al
        movzx eax, al
        jmp elog
eow:
        mov ax, [esi]
        out dx, ax
        movzx eax, ax
        jmp elog
eod:
        mov eax, [esi]
        out dx, eax
        jmp elog
ein:
        test cl, DWORD_IO
        jnz eid
        test cl, WORD_IO
        jnz eiw
        in al, dx
        mov [esi], al
        movzx eax, al
        jmp elog
eiw:
        in ax, dx
        mov [esi], ax
        movzx eax, ax
        jmp elog
eid:
        in eax, dx
        mov [esi], eax
elog:
        call data_log                   ; EAX = element
        mov eax, s_step
        add s_off, eax
        dec s_left
        jmp elem
sdone:
        mov eax, s_off                  ; SI / DI and CX back to the guest
        test cl, OUTPUT
        jz @F
        mov word ptr [ebp].Client_Reg_Struc.Client_ESI, ax
        jmp sreg
@@:
        mov word ptr [ebp].Client_Reg_Struc.Client_EDI, ax
sreg:
        test cl, REP_IO
        jz @F
        mov word ptr [ebp].Client_Reg_Struc.Client_ECX, 0
@@:
        jmp iexit
iocb endp

;--- count_log: EBX = header (0 = none), EDX = port, CL = type,
;---            SI = value, EDI = kind 0-4 or -1 (not counted).
;---            Preserves everything.
count_log proc
        or ebx, ebx
        jz cr
        push eax
        push edi
        inc dword ptr [ebx+H_NEV]
        cmp edi, -1
        jz nocount
        movzx eax, dx
        sub ax, word ptr [ebx+H_W0]
        cmp eax, 16
        jb idx
        movzx eax, dx
        sub ax, word ptr [ebx+H_W1]
        cmp eax, 8
        jae nocount
        add eax, 16
idx:
        lea eax, [eax*4+eax]
        add eax, edi
        inc dword ptr [ebx+H_COUNTS+eax*4]
nocount:
        call may_log
        jc cd
        call put_entry
cd:
        pop edi
        pop eax
cr:
        ret
count_log endp

;--- data_log: one string element in EAX (a dword logs two entries).
;---           Honours strmax.  Preserves everything.
data_log proc
        or ebx, ebx
        jz dr
        call may_log
        jc dr
        push eax
        push ecx
        push esi
        movzx esi, word ptr [ebx+H_STRMAX]
        or esi, esi
        jz @F
        cmp s_nlog, esi
        jae dx_
@@:
        inc s_nlog
        and cl, 1Ch                     ; size + direction
        or cl, 80h                      ; data element
        movzx esi, ax
        call put_entry
        test cl, DWORD_IO
        jz dx_
        shr eax, 16
        movzx esi, ax
        call put_entry
dx_:
        pop esi
        pop ecx
        pop eax
dr:
        ret
data_log endp

;--- may_log: CF = do not log (disarmed, or port filtered).  Preserves all.
may_log proc
        push eax
        push edi
        cmp byte ptr [ebx+H_ARMED], 0
        jz nolog
        movzx eax, byte ptr [ebx+H_NFILT]
        lea edi, [ebx+H_FILT]
fl:
        or eax, eax
        jz yes
        cmp dx, [edi]
        jz nolog
        add edi, 2
        dec eax
        jmp fl
yes:
        pop edi
        pop eax
        clc
        ret
nolog:
        pop edi
        pop eax
        stc
        ret
may_log endp

;--- put_entry: EBX header, EDX port, CL type, SI value.  An exact repeat of
;---            the previous entry bumps its repeat count.  Preserves all.
put_entry proc
        push eax
        push edi
        mov eax, [ebx+H_NLOG]
        or eax, eax
        jz pnew
        dec eax
        imul eax, eax, ENTSZ
        movzx edi, word ptr [ebx+H_BUFSEG]
        shl edi, 4
        add edi, eax
        cmp [edi], dx
        jnz pnew
        cmp [edi+2], si
        jnz pnew
        cmp [edi+4], cl
        jnz pnew
        cmp byte ptr [edi+5], 0FFh
        jz pnew
        inc byte ptr [edi+5]
        jmp px
pnew:
        mov eax, [ebx+H_NLOG]
        cmp eax, [ebx+H_BUFCAP]
        jb proom
        inc dword ptr [ebx+H_NDROP]
        jmp px
proom:
        imul eax, eax, ENTSZ
        movzx edi, word ptr [ebx+H_BUFSEG]
        shl edi, 4
        add edi, eax
        mov [edi], dx
        mov [edi+2], si
        mov [edi+4], cl
        mov byte ptr [edi+5], 0
        mov ax, word ptr ds:[46Ch]      ; BIOS tick, low word
        mov [edi+6], ax
        mov al, 0C2h                    ; 8254 read-back: count + status, ch0
        out 43h, al
        in al, 40h
        mov [edi+10], al
        mov byte ptr [edi+11], 0
        in al, 40h
        mov ah, al
        in al, 40h
        xchg al, ah
        mov [edi+8], ax
        inc dword ptr [ebx+H_NLOG]
px:
        pop edi
        pop eax
        ret
put_entry endp

;--- JLM load / unload --------------------------------------------------------
DllMain proc stdcall public hModule:dword, dwReason:dword, dwRes:dword
        mov eax, dwReason
        cmp eax, DLL_PROCESS_DETACH
        jnz @F
        call disarm
@@:
        mov eax, 1
        ret
DllMain endp

        end DllMain
