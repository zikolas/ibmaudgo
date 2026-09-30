# IBMAUDGO — DOS enabler for the IBM PCMCIA Audio Adapter

A clean-room DOS enabler for the IBM PCMCIA Audio Adapter (P/N 0933967, the
card whose CIS reads `IBM | NON-DSP AUDIO`, MANFID `00A4/0022`), built in the
same shape as our other unified enablers
([ES1688GO](https://github.com/zikolas/es1688go),
[SCP55GO](https://github.com/zikolas/scp55-enabler),
[MC8KGO](https://github.com/zikolas/mc8kgo)): one `.COM`, three host backends.

    IBMAUDGO [/PCIC|/CS|/OB] [/IO1=250] [/IO2=340] [/I=n] [/S=n]
             [/W=D000] [/FORCE] [/OFF] [/?]

* `/PCIC` — Intel 82365-class point enabler, direct PCIC programming
* `/CS`   — PCMCIA Card Services 2.1 client, stays TSR (hot-plug, live reconfigure)
* `/OB`   — HP OmniBook 300/425/430 Socket Services direct

With no mode switch the host is auto-detected: Card Services first, then the
Socket Services `SS` signature, then an 82365 probe at `3E0h`.

* `/IO1=hex` — control block base: `250`, `650`, `A50` or `E50` (default 250)
* `/IO2=hex` — data block base: `340`, `740`, `B40` or `F40` (default: the one
  paired with `/IO1`)
* `/I=dec` — route the card's interrupt to IRQ 5, 10, 11 or 15 (default: none)
* `/S=dec` — socket: PCIC 0-7, OmniBook 1-2; under Card Services, probe only
  this socket
* `/W=hex` — segment of the attribute-memory window used to read the CIS and
  write the COR (PCIC; default `D000`)
* `/FORCE` — configure without the CIS identity check (needs `/S`)
* `/OFF` — power the socket down (PCIC), or release the card and go dormant (CS)

Typical use, with the window segment kept out of the memory manager's upper
memory (for Jemm, `X=DC00-DFFF`):

    IBMAUDGO /W=DC00

Build with NASM, on a modern host or under DOS:

    nasm -f bin IBMAUDGO.ASM -o IBMAUDGO.COM

## What the card is

A WAV player. An IBM ASIC in front of a serial codec keeps a 16K-word sample
ring on the card; software appends samples to it with word writes to the data
block and reads back how far the card has played. The card has no Sound Blaster
logic, no DMA and no FM chip, and the MIDI in IBM's Windows software is a
synthesiser running on the host CPU. The interface was recovered by I/O trace
of IBM's own DOS WAV player and is written up in [doc/recon.md](doc/recon.md).

IBMAUDGO only configures the card, as IBM's `PCAUDDD.SYS` does. Two programs
play it once IBMAUDGO has run:

* `probes/IBMPLAY` plays a PCM WAV file: 8- or 16-bit, mono or stereo, at a
  rate from the codec's table.
* [VSBPCMCIA](https://github.com/zikolas/vsbpcmcia)'s `/CARD:IBMAUD` backend
  (from v1.3-pre1) emulates a Sound Blaster on it for games: DOOM and Epic
  Pinball on an IBM PC110, and Monkey Island's AdLib music through its software
  OPL build on a Toshiba T2130CT.

## What the card needs

The CIS is not a useful guide to this card's I/O. Its `CFTABLE_ENTRY`
declares `201h-336h`, a hull around the real decode. The card decodes ten
address lines, so it answers four aliases, and the vendor's own configuration
tool names the real pairs:

| control block (16 bytes) | data block (8 bytes) |
|---|---|
| `250h-25Fh` | `340h-347h` |
| `650h-65Fh` | `740h-747h` |
| `A50h-A5Fh` | `B40h-B47h` |
| `E50h-E5Fh` | `F40h-F47h` |

`/IO1` picks the alias and the data block is paired automatically; `/IO2`
only needs giving to break that pairing. The control block takes the codec's
control frames and the mode registers; the data block takes the sample stream
and reports status and the play position. The data block's aliases include
`F40h`, a classic Windows Sound System base, but it holds no WSS registers;
0.3 and earlier called it the WSS block.

Other card facts the enabler relies on: config registers at attribute `FFF0h`
(above the 16 KB probe window, so the attribute window is re-pointed at the
COR's own 4 KB page before the COR is written), a single config entry at index
1, COR written as `41h` (index 1 + level-mode IREQ), and Vcc only. The card has
no Vpp, so the Vpp rails stay off in both the PCIC and the Card Services paths.

No IRQ is routed unless `/I=n` asks for one: 5, 10, 11 or 15, the levels IBM's
documentation lists (the CIS allows any). Once routed, the card interrupts
every 8.19 ms, idle or not. IBM's DOS driver paces on that, but its Windows
driver runs without one, and the VSBPCMCIA backend polls the play position
from its own timer, so nothing here needs it. 0.2 routed IRQ 10 by default.
The HP OmniBook Socket Services refuse an I/O socket with no IRQ steering, so
under `/OB` the card is steered to 10 anyway and reported as having none.

## Status

* PCIC path — bench-proven on an IBM PC110 (2026-09-02) and on the ToPIC
  controller of a Toshiba T2130CT (2026-09-30). Identifies the card,
  writes the COR at `FFF0h` and reads it back verified, and brings both windows
  up decoding. With 0.2's default IRQ 10, the socket read back independently
  matches the running vendor stack on every register that matters: `intctl
  EA`, `ioctl B2`, `winen C0`, io win0 `0250-025F`, io win1 `0340-0347`, COR
  `41`, CCSR `00`. Power is `B0h` where the vendor writes `F0h`; both leave Vpp
  off. Verified cold-socket, and re-running it on an already-configured card
  works too. With no IRQ (`intctl 60`, 0.3's default) it is what the
  VSBPCMCIA backend runs on.
* CS path — adapted, not yet bench-run.
* OB path — unverified; no OmniBook has had this card.

## Repository

* `IBMAUDGO.ASM`, `IBMAUDGO.COM` — the enabler (`build.sh` assembles it)
* `doc/recon.md` — the recon log: what the card is, its resource map, the
  enable recipe read back from the vendor stack, the register-file hunt that
  went nowhere, and the playback interface from the I/O trace
* `probes/` — the bring-up, snapshot and trace tools, and IBMPLAY; see
  [probes/README.md](probes/README.md)

IBM's driver diskettes, which the recon read as data, are not in the
repository.

## Licence

GNU General Public License v2 — see `LICENSE`.

Clean-room provenance: the card's own CIS read off the hardware; the resource
map and IRQ list as stated in plain text by the vendor's shipped documentation
and configuration tool; the socket state read back from a running vendor stack;
the public Intel 82365SL register set; the PCMCIA CS/SS specs via RBIL61 and
the SystemSoft CardSoft technical guide; and HP OmniBook Socket Services
behaviour probed live. The playback interface used by IBMPLAY and the
VSBPCMCIA backend comes from logging the vendor driver's port accesses while
it played test files. No vendor driver was disassembled. Structure forked from
our own MC8KGO 0.1 / SCP55GO 2.1.
