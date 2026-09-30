# probes

The tools written while working the card out, roughly in the order they were
needed. None of them is needed to use the card: IBMAUDGO configures it, and
IBMPLAY or VSBPCMCIA plays it. [../doc/recon.md](../doc/recon.md) has what each
one found.

| probe | what it does |
|---|---|
| `IBM16TRY.C` | first bring-up: CIS gate, COR, two I/O windows, codec hunt |
| `IBM16TR2.C` | decode-vs-no-answer control: which reads come from the card |
| `IBM16TR3.C` | full-map sweep |
| `IBM16TR4.C` | host/card attribution |
| `IBM16TR5.C` | codec identification |
| `IBM16TR6.C` | read/write map |
| `IBMSNAP.C` | read-only snapshot of the card as the vendor stack leaves it (`/W` watch, `/O` open the driver's device first, `/E` self-enable, `/H` codec hunt) |
| `CARDPWR.C` | powers a socket off or on, to clear a card's stale COR after a warm reboot |
| `IBMTRACE.ASM` | QPI port-trap TSR that logs every access to both port blocks |
| `IBMTRJ.ASM` | the same trap at ring 0 as a Jemm JLM, so `REP OUTSW` is performed and logged in full |
| `ibmtrace.py` | decodes an IBMTRACE `/W` log on the host |
| `TRTEST.ASM`, `TRTEST2.ASM` | pass-through checks: run with and without the trap, the output must match |
| `IBMPLAY.C` | plays a PCM WAV file on the card with no vendor driver |

## Building

The C probes are Open Watcom, 16-bit DOS, small model; they were built under
DOS with a local batch file. IBMPLAY uses `REP OUTSW`, a 186 instruction, so it
needs `-1`; on a host:

    wcl -ms -1 -zq -bt=dos -fe=IBMPLAY.EXE IBMPLAY.C

The assembler probes are NASM:

    nasm -f bin IBMTRACE.ASM -o IBMTRACE.COM
    nasm -f bin TRTEST.ASM -o TRTEST.COM
    nasm -f bin TRTEST2.ASM -o TRTEST2.COM

IBMTRJ is MASM syntax and builds with Open Watcom's JWasm and wlink. `pe2px.py`
checks the DLL against what Jemm's JLOAD requires of a module and changes its
signature from PE to PX:

    jwasm -c -coff -nologo -I<dir of JLM.INC> -FoIBMTRJ.OBJ IBMTRJ.ASM
    wlink @IBMTRJ.LNK
    python3 pe2px.py IBMTRJ.DLL

`JLM.INC` is Jemm's include file (public domain,
github.com/Baron-von-Riedesel/Jemm) and is not in this repository.

Binaries are built from these sources and are not committed.
