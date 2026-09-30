# IBM PCMCIA Audio Adapter — recon log

Card: **IBM "NON-DSP AUDIO" PC Card**, P/N 0933967, MANFID **00A4/0022**
(0x00A4 is IBM — the Microdrive is 00A4/0000). Vendor name in the manual is
"PCMCIA Audio Adapter", First Edition November 1994. Ships with an external
**AIM** (Audio Interface Module) dongle carrying the jacks.

Bench: IBM PC110, Intel-compatible PCIC at 0x3E0, socket 0.

Status (2026-09-30): solved. The playback interface is in "The playback
interface" below; `probes/IBMPLAY` plays WAV files with it and VSBPCMCIA's
`/CARD:IBMAUD` backend emulates a Sound Blaster on it. The register-file hunt
is kept as the record of a dead end.

## What the card is

From the vendor's own `AUDIODOC.TXT` and `WINAUDIO.WRI` — read as data, no
disassembly:

- 8- and 16-bit digital audio, mono/stereo, record and playback.
- **Fourteen sample rates**: 5512, 6615, 8000, 9600, 11025, 16000, 18900,
  22050, 27428, 32000, 33075, 37800, 44100, 48000 Hz. That is *exactly* the
  AD1848 / CS4231 rate table, both crystal columns — so the analog part is a
  Crystal-lineage WSS codec, as suspected.
- Release 2.1 of the Windows software adds "Support for Microsoft Windows
  Sound System 2.0" and wavetable MIDI **synthesised in software** (DISK2 is
  ~200 `.SMP`/`.16` instrument samples). That is what "NON-DSP" means: no
  Mwave DSP, the MIDI is rendered on the host CPU — hence the manual's
  "we recommend a 486 if you intend to play MIDI files".
- Requires **Card and Socket Services 2.00 or higher**, but can also be
  "point enabled" without them on an Intel-compatible controller.

## Resource map — settled

The CIS `CFTABLE_ENTRY` declares two I/O ranges, 0x201–0x336 and 0x340–0x346,
with a **10 address-line** decode. The first range is a *hull*, not the real
decode. The vendor's `CONFIG.EXE` states it exactly:

> The Audio Adapter requires 1 of these 4 IO port range pairs:
> `250H-25FH and 340H-347H` / `650H-65FH and 740H-747H` /
> `A50H-A5FH and B40H-B47H` / `E50H-E5FH and F40H-F47H`

All four pairs share identical low-10 bits, which confirms the 10-line decode:
the card answers any alias, and the host picks one. `F40H-F47H` is the classic
Windows Sound System base — so the 8-byte block is the WSS block and the
16-byte block is IBM's own. (Superseded by the I/O trace below: the 8-byte
block holds no WSS registers. It is the data block, sample FIFO, status and
play position, and the 16-byte block is the control block. IBMAUDGO uses
those names from 0.4.)

`AUDIODOC.TXT` agrees: "requires the exclusive use of the memory from 250-25F
and 340-347". Without Card Services the card is point-enabled through an
attribute window at **CF00-CFFF**, which must be excluded from any memory
manager.

- COR at attribute **0xFFF0**, CCSR at 0xFFF2, single config, **index 1**.
- IRQ default **10**; 5, 11 and 15 also supported.
- 5V, Vcc only — **no Vpp** (see the warning below).

## The enable recipe — captured and reproduced

`IBMSNAP` was run with the full vendor stack resident (IBM Socket Services
2.1 `SSDPCIC1.SYS` + IBM Card Services 2.10 `IBMDOSCS.SYS` + `PCAUDDD.SYS
/IRQ=10`) and with the driver's `AUDIO1$` device held open, then read the
socket back. The configuration it chose:

| PCIC reg | value | meaning |
|---|---|---|
| 0x02 power   | `F0` | card power + auto-power + output enable, **Vpp OFF** |
| 0x03 intctl  | `EA` | I/O card, reset released, IRQ 10 |
| 0x06 winen   | `E1` | mem win0 + io win0 + io win1 |
| 0x07 ioctl   | `B2` | win0 IOCS16-from-card; win1 16-bit data + IOCS16 + wait |
| io win0      | `0250-025F` | |
| io win1      | `0340-0347` | |
| mem win0     | 4K at `0xCC000`, offset `0x3F43`+REG# → card attr page 0x0F |
| COR @ 0xFFF0 | `41` | config index 1 **+ level-mode IREQ** |

`IBMSNAP /E` reproduces all of that from our own code with no driver and no
Card Services, and the resulting register dump is byte-identical to the
vendor's apart from two memory-window bits. **The enabler side is solved.**

## The register-file hunt — a dead end (closed 2026-09-29)

With the card configured (ours or the vendor's), no base in either block
behaves like an AD1848 Index Address Register:

- `0x258`, `0x259`, `0x25F`, and under the vendor stack also `0x254`, accept
  any byte and return it — but reading indices 0..15 returns *the same value
  every time*. That is a plain latch, not an indexed register file. An early
  "I0 scratch AA->AA 55->55" reading looked like a live codec and is not;
  the discriminator that matters is whether different indices differ.
- `0x25A` reads `A0` constantly, `0x342` reads `1A` with bit 7 read/write.
- `0x344` (WSS base+4, the textbook AD1848 spot) reads `00` and does not
  echo an index — **even while `PCDIAG` is generating a tone**.

So the DOS stack drives a proprietary IBM interface, not a WSS-compatible
one. The card is healthy: `PCDIAG` passes its self-test and generates L/R
tones.

**Next lead.** `CARDWAVE.DRV` from the Windows disk decompresses to
*Microsoft's stock Windows Sound System driver* — its strings mention Compaq
Business Audio, `VDMAD.386`, DMA channel pairs and "Windows Sound System
Playback/Record/Mixer". A stock MS WSS driver only works against a real
AD1848-class codec at a WSS base. So the card has a WSS personality that the
DOS stack never selects — exactly the pattern that cracked the CF-VEW212.
The next step is to bring up the Windows stack (or find the bit that selects
WSS mode) and re-read the card, rather than sweeping bases blind.

Note the MS driver talks about **DMA channels**. PCMCIA 16-bit DMA needs host
support the 82365 does not have, so how the Windows path moves samples is an
open question that matters for any vsbpcm backend.

## The playback interface, from an I/O trace of the vendor driver (2026-09-29)

The WSS lead above is closed: no host-visible codec register file exists
under either vendor stack. The interface was instead recovered by trapping
both port blocks while the vendor DOS driver (`PCAUDDD.SYS` 1.10 under IBM
Socket/Card Services) played known test files with the shipped `PLAYFILE.EXE`.
Tools: `probes/IBMTRACE.ASM` (logger, DOS side) and `probes/IBMTRJ.ASM` (a
Jemm JLM that traps the ports at ring 0 and performs each access, including
`REP OUTSW`, exactly). Nothing was disassembled; this is the card's observed
I/O behaviour. Test files: 1 s tones at 8000 Hz mono 8-bit, 11025 Hz mono
16-bit, 22050 Hz stereo 16-bit. Ports below use the default alias 250h/340h.

### Data path

- `340h` (word, write): the sample stream. The driver writes raw WAV PCM with
  `REP OUTSW` in blocks: 8-bit unsigned samples packed two per word (first
  sample in the low byte), 16-bit signed samples one per word, stereo
  interleaved L,R. The whole payload of each test file arrived byte-exact.
- The card holds a 16K-word (32 KB) ring. `346h` (word, read) is its read
  position in words, wrapping at `4000h`. The write position is implicit
  (each word written to `340h` advances it). There is no FIFO-full flag; the
  driver paces by comparing its own write count with `346h`.
- The card raises a periodic interrupt (IRQ 10 as steered) every 8.19 ms,
  also while idle. `342h` bit 8 is the pending flag and clears on read
  (`011Ah` then `001Ah`). Per tick the driver reads `346h` and, if there is
  room, writes one block: 32 words at 8000 mono 8-bit, 90 at 11025 mono
  16-bit, 360 at 22050 stereo 16-bit (one tick's worth). It prefills 20
  blocks at start.
- `342h` low byte reads `1Ah` idle and `0Ah` while playing (bit 4 = idle).

### Control

- `342h` (word, write) takes commands: `0200` at open and at close, `0001`
  then `0002` before codec setup, `0302` to prepare, `0102` to start.
- `250h`-`254h` are a five-byte codec control frame, sent when `258h` is
  written with bit 7 set; `259h` is pulsed `86h` then `06h` between frames.
  Reading `250h` back returns the codec's status byte.
  - `251h` is the data format: bits 1:0 format (11 = 8-bit unsigned,
    00 = 16-bit linear), bit 2 stereo, bits 5:3 rate select.
  - `252h` bits 5:4 select the crystal: 01 = 24.576 MHz, 10 = 16.9344 MHz
    (`96h` / `A6h` observed).
  - Observed: 8000 mono 8-bit `03`/`96`; 11025 mono 16-bit `08`/`A6`;
    22050 stereo 16-bit `1C`/`A6`. The rate selects give exactly those rates
    from the Crystal rate table on those crystals.
  - Handshake: frames with `250h` = `00` repeat until the status read back
    shows bit 2 clear (`20h`), then frames with `250h` = `04` until it shows
    bit 2 set (`24h`), then `258h` leaves control mode. `25Ch` reads `03`
    at those checks.
  - These fields match the published control-word layout of the Crystal
    CS4215 / AD1849 serial codec (data format register, serial control
    register, control latch bit). Working hypothesis: the ASIC shifts these
    bytes into such a codec. The chip on the board is unconfirmed.
- `258h` is the mode register. Written `88h`/`A8h`/`B8h` in control mode
  (bit 5 = 16-bit, bit 4 = stereo), then `04h`/`24h`/`34h` for data mode,
  then `01h` to run; it reads back `05h`/`35h` while playing.
- In data mode `254h`-`257h` carry the output settings. `254h` = headphone
  enable (bit 7) + left attenuation (bits 5:0), `255h` = right attenuation,
  `256h` input gains, `257h` monitor. Playing: `80 00 00 F0`. Idle:
  `BE 3E 0F FF`. The driver ramps the attenuation one step at a time on
  every transition (anti-pop).
- Start: `342h`=`0302`, `259h`=`1E`, `258h`=mode, `25Fh`=`FF`, `259h`=`86`,
  two silence words to `340h`, `258h`=`01`, `342h`=`0102`, then blocks.
- Stop: silence words on the following ticks, fade out, `342h`=`0200`.
- After the prefill the driver writes `258h`=`05h` (+ format bits) and
  `259h`=`06h`. `259h` bit 7 holds playback: without this write the card
  reports playing (`342h`=`0Ah`) but `346h` stays at 0.

### Confirmed by our own player (2026-09-29)

`probes/IBMPLAY.C` plays WAV files on the PC110 with only `IBMAUDGO /PCIC`
loaded, using the sequence above and polling `346h`. Consumption rates
measured over the file (BIOS-tick resolution): 8000 mono 8-bit, 11025 mono
16-bit, 22050 stereo 16-bit, 44100 stereo 16-bit and 48000 stereo 16-bit all
match their files, so the rate-select and crystal fields extend beyond the
three traced rates as the Crystal table predicts. The PC110 keeps up with
44.1 kHz stereo from disk with `REP OUTSW`.

- Keep the ring less than 8K words ahead of `346h`. Queuing 8192 words
  overflowed it: the card went idle (`342h`=`1Ah`), the codec status read
  `FFh` and `346h` stopped being monotonic. 4096 ahead is safe; the exact
  limit is not yet measured.
- The close command does not flush the ring: queued words still play if
  the hold is released afterwards.

## Warnings

- **Vpp**: this card is Vcc-only. Powering a socket with `0x95` (Vpp1/Vpp2 =
  Vcc) puts the flash programming rail on an audio card for no reason. Use
  `0xB0`/`0xF0`-class values. `CISDUMP.C` and `SCPROBE.C` both use `0x95`
  unconditionally and should be fixed.
- A PC Card's COR is volatile but lives **on the card**, so a warm reboot
  leaves a stale config behind: the host PCIC resets, the card does not. The
  vendor tools then report "Audio Adapter Not Found". Only removing Vcc
  clears it — `CARDPWR.C` does that, or eject/reinsert.
- `PCDIAG` needs Card and Socket Services resident; it fails with
  "Audio Adapter Not Found" without them. Its panel keys are
  **1** = left tone, **2** = right tone, **3** = exit; it runs in a graphics
  mode, so a text-screen capture shows nothing.

## Files

`probes/` — `IBM16TRY` first bring-up, `IBM16TR2` decode-vs-no-answer control,
`IBM16TR3` full-map sweep, `IBM16TR4` host/card attribution, `IBM16TR5` codec
identification, `IBM16TR6` read/write map, `IBMSNAP` vendor readback +
`/E` self-enable + `/H` codec hunt, `CARDPWR` socket power control,
`IBMTRACE` + `IBMTRJ` I/O trace (decoder `ibmtrace.py`, JLM build helper
`pe2px.py`, link file `IBMTRJ.LNK`), `TRTEST`/`TRTEST2` trace self-checks,
`IBMPLAY` WAV player. Build notes in `probes/README.md`.
`vendor/` — the shipped driver diskettes, kept out of the repository.
