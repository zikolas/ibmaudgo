# IBMAUDGO — DOS enabler for the IBM PCMCIA Audio Adapter

A clean-room DOS enabler for the IBM PCMCIA Audio Adapter (P/N 0933967, the
card whose CIS reads `IBM | NON-DSP AUDIO`, MANFID `00A4/0022`), built in the
same shape as our other unified enablers: one `.COM`, three host backends.

    IBMAUDGO [/PCIC|/CS|/OB] [/IO1=250] [/IO2=340] [/I=n] [/S=n]
             [/W=D000] [/FORCE] [/OFF] [/?]

* `/PCIC` — Intel 82365-class point enabler, direct PCIC programming
* `/CS`   — PCMCIA Card Services 2.1 client, stays TSR (hot-plug, live reconfigure)
* `/OB`   — HP OmniBook 300/425/430 Socket Services direct

With no mode switch the host is auto-detected: Card Services first, then the
Socket Services `SS` signature, then an 82365 probe at `3E0h`.

Build, on a host or on the box:

    nasm -f bin IBMAUDGO.ASM -o IBMAUDGO.COM

## What the card is

A WAV player. An IBM ASIC in front of a serial codec keeps a 16K-word sample
ring on the card; software appends samples to it with word writes to window 1
and reads back how far the card has played. The card has no Sound Blaster
logic, no DMA and no FM chip, and the MIDI in IBM's Windows software is a
synthesiser running on the host CPU. The interface was recovered by I/O trace
of IBM's own DOS WAV player and is written up in [doc/recon.md](doc/recon.md).

IBMAUDGO only configures the card, as IBM's `PCAUDDD.SYS` does. Two programs
play it once IBMAUDGO has run:

* `probes/IBMPLAY` plays a PCM WAV file: 8- or 16-bit, mono or stereo, at a
  rate from the codec's table.
* VSBPCMCIA's `/CARD:IBMAUD` backend emulates a Sound Blaster on it for games.
  DOOM and Epic Pinball play on an IBM PC110.

## What the card needs

The CIS is not a useful guide to this card's I/O. Its `CFTABLE_ENTRY`
declares `201h-336h`, a hull around the real decode. The card decodes ten
address lines, so it answers four aliases, and the vendor's own configuration
tool names the real pairs:

| window 0 (16 bytes) | window 1 (8 bytes) |
|---|---|
| `250h-25Fh` | `340h-347h` |
| `650h-65Fh` | `740h-747h` |
| `A50h-A5Fh` | `B40h-B47h` |
| `E50h-E5Fh` | `F40h-F47h` |

`/IO1` picks the alias and window 1 is paired automatically; `/IO2` only needs
giving to break that pairing. `F40h` is the classic Windows Sound System base,
which is what the four aliases give away: window 1 sits where a WSS block
would.

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

* PCIC path — bench-proven on an IBM PC110 (2026-09-02). Identifies the card,
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
