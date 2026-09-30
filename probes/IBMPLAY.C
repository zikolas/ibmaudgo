/* IBMPLAY.C - play a WAV on the IBM PCMCIA Audio Adapter with no vendor
 * driver: the card enabled by IBMAUDGO, then driven from the interface
 * recovered by I/O trace (doc/recon.md, 2026-09-29).
 *
 *   IBMAUDGO /PCIC
 *   IBMPLAY file.wav [/IO=250] [/AHEAD=n] [/REPEAT=n]
 *
 * /REPEAT=n streams the file n more times after the card has run dry and
 * stalled, with no control writes in between: does the card resume by
 * itself when words arrive after an underrun?
 *
 * PCM WAV only: 8-bit unsigned or 16-bit signed, mono or stereo, at a rate
 * in the codec table.  The card keeps a 16K-word ring: words written to
 * base1+0 (340h) append to it, base1+6 (346h) reads back its play position
 * in words.  So playback is: set the codec up, start, and keep the ring a
 * few thousand words ahead of 346h.  No IRQ is used.
 *
 * Measured at the end: the rate the card consumed words at.  If that
 * matches the file, the codec is clocked as programmed.
 *
 * Only 8000 mono 8-bit, 11025 mono 16-bit and 22050 stereo 16-bit were
 * traced.  The other table rates follow the same field layout (Crystal
 * CS4215/AD1849 data format register) and are unverified until played.
 *
 * Build (host):  wcl -ms -1 -zq -bt=dos -fe=IBMPLAY.EXE IBMPLAY.C
 *
 * (C) 2026 zikolas.  GNU General Public License v2 - see LICENSE.
 * Original work; register semantics are the card's observed behaviour.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>

static unsigned b0 = 0x250, b1 = 0x340;
#define R_FIFO   (b1 + 0)          /* word: sample stream              */
#define R_CMD    (b1 + 2)          /* word: status / command           */
#define R_POS    (b1 + 6)          /* word: ring play position (words) */
#define RING     0x4000u           /* ring size in words               */

static void outsw_near(unsigned port, const unsigned short *buf, unsigned n);
#pragma aux outsw_near = \
    "cld" \
    "rep outsw" \
    parm [dx] [si] [cx] \
    modify [si cx];

static unsigned long ticks(void)
{
    return *(unsigned long __far *)MK_FP(0x40, 0x6C);
}

/* busy delay of roughly n microseconds: an ISA port read is ~1 us */
static void udelay(unsigned n)
{
    while (n--) inp(0x61);
}

/* Codec rate table: rate, crystal (1 = 24.576 MHz, 2 = 16.9344 MHz), rate
 * select.  Crystal Semiconductor rate table, as in the vendor manual. */
static const struct { unsigned rate; unsigned char xtal, dfs; } rtab[] = {
    { 8000, 1, 0 }, { 16000, 1, 1 }, { 27429, 1, 2 }, { 32000, 1, 3 },
    { 48000, 1, 6 }, { 9600, 1, 7 },
    { 5513, 2, 0 }, { 5512, 2, 0 }, { 11025, 2, 1 }, { 18900, 2, 2 },
    { 22050, 2, 3 }, { 37800, 2, 4 }, { 44100, 2, 5 }, { 33075, 2, 6 },
    { 6615, 2, 7 },
};

static unsigned char dfr, scr, mbits;   /* 251h, 252h, 258h format bits */

/* one five-byte control frame, strobed by 258h bit 7 */
static unsigned char ctl_frame(unsigned char clb, int pulse)
{
    outp(b0 + 0, clb);
    outp(b0 + 1, dfr);
    outp(b0 + 2, scr);
    outp(b0 + 3, 0);
    outp(b0 + 4, 0);
    if (pulse) { outp(b0 + 9, 0x86); outp(b0 + 9, 0x06); }
    outp(b0 + 8, 0x88 | mbits);
    udelay(100);
    return (unsigned char)inp(b0 + 0);
}

static void ramp(unsigned port, unsigned from, unsigned to)
{
    if (from <= to) { while (from <= to) outp(port, from++); }
    else            { while (from >= to) { outp(port, from); if (from-- == to) break; } }
}

static int codec_setup(void)
{
    unsigned char st = 0;
    int i;

    outpw(R_CMD, 0x0200);
    ramp(b0 + 5, 0x00, 0x3E);           /* mute right, then left */
    ramp(b0 + 4, 0x00, 0x3E);
    outpw(R_CMD, 0x0001);
    outpw(R_CMD, 0x0002);

    /* control latch bit 0 until the codec echoes it (status 20h) */
    for (i = 0; i < 40; i++) {
        st = ctl_frame(0x00, i > 0);
        if ((st & 0x24) == 0x20) break;
    }
    printf("  control mode: %d frame(s), status %02X, 25C=%02X\n",
           i + 1, st, inp(b0 + 0x0C));
    if (i == 40) return -1;

    /* then 1 until it echoes that (status 24h) */
    for (i = 0; i < 40; i++) {
        st = ctl_frame(0x04, 1);
        if (i == 0) { udelay(5000); continue; }
        if ((st & 0x24) == 0x24) break;
    }
    printf("  latch set:    %d frame(s), status %02X, 25C=%02X\n",
           i + 1, st, inp(b0 + 0x0C));
    if (i == 40) return -2;

    outp(b0 + 8, 0x04 | mbits);         /* data mode */
    outp(b0 + 9, 0x06);
    return 0;
}

static void prepare(unsigned short silence)
{
    int i;
    outp(b0 + 4, 0x3F); outp(b0 + 5, 0x3F);
    outp(b0 + 6, 0x0F); outp(b0 + 7, 0xFF);
    outpw(R_FIFO, silence); outpw(R_FIFO, silence);
    for (i = 0; i < 1001; i++) outp(b0 + 8, 0x01);
    outp(b0 + 8, 0x04 | mbits);
    outpw(R_CMD, 0x0302); outpw(R_CMD, 0x0302);
    outp(b0 + 9, 0x1E);
    outp(b0 + 8, 0x04 | mbits);
    outp(b0 + 0x0F, 0xFF);
    outp(b0 + 9, 0x86); outp(b0 + 9, 0x86);
}

/* arm: the ring is then filled while 259h bit 7 holds playback */
static void start(unsigned short silence)
{
    outpw(R_CMD, 0x0302);
    outp(b0 + 9, 0x1E);
    outp(b0 + 8, 0x04 | mbits);
    outp(b0 + 0x0F, 0xFF);
    outp(b0 + 9, 0x86);
    outpw(R_FIFO, silence); outpw(R_FIFO, silence);
    outp(b0 + 8, 0x01);
    outpw(R_CMD, 0x0102);
}

/* release the hold: the card starts consuming the ring */
static void go(void)
{
    outp(b0 + 8, 0x05 | mbits);
    outp(b0 + 9, 0x06);
    outp(b0 + 6, 0x00); outp(b0 + 7, 0xF0);
    ramp(b0 + 5, 0x3E, 0x00);           /* fade in right, then left */
    ramp(b0 + 4, 0xBE, 0x80);
}

static void finish(void)
{
    ramp(b0 + 5, 0x00, 0x3E);           /* fade out, then close */
    ramp(b0 + 4, 0x80, 0xBE);
    outpw(R_CMD, 0x0200);
}

/* consumed-word counter from the 14-bit ring position */
static unsigned lastpos;
static unsigned long consumed;
static void track(void)
{
    unsigned p = inpw(R_POS) & (RING - 1);
    consumed += (p - lastpos) & (RING - 1);
    lastpos = p;
    (void)inpw(R_CMD);                  /* reading clears the tick flag */
}

static unsigned short buf[2048];

int main(int argc, char **argv)
{
    FILE *f;
    char hdr[12], id[5];
    unsigned long len, sz, written = 0, t0, t1, c0, dataend = 0;
    unsigned fmt = 0, ch = 0, rate = 0, bits = 0, ahead = 4096, repeat = 0, pass;
    long datapos;
    unsigned long datalen;
    unsigned short silence;
    const char *name = 0;
    int i, found = 0, aborted = 0, padded = 0;

    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/IO=", 4)) { b0 = (unsigned)strtoul(argv[i] + 4, 0, 16); b1 = (b0 & 0xFC00) | 0x340; }
        else if (!strnicmp(argv[i], "/AHEAD=", 7)) ahead = (unsigned)strtoul(argv[i] + 7, 0, 10);
        else if (!strnicmp(argv[i], "/REPEAT=", 8)) repeat = (unsigned)strtoul(argv[i] + 8, 0, 10);
        else name = argv[i];
    }
    if (!name) { printf("IBMPLAY file.wav [/IO=250] [/AHEAD=4096]\n"); return 1; }
    if (ahead < 256 || ahead > RING - 1024) ahead = 4096;
    if ((b0 & 0x3F0) == 0x3E0 || (b1 & 0x3F8) == 0x3F8) { printf("bad /IO\n"); return 1; }

    f = fopen(name, "rb");
    if (!f) { printf("cannot open %s\n", name); return 1; }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        printf("not a RIFF WAVE file\n"); return 1;
    }
    id[4] = 0;
    for (;;) {                           /* walk chunks to fmt and data */
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) { printf("no data chunk\n"); return 1; }
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fb[16];
            fread(fb, 1, 16, f);
            fmt = fb[0] | (fb[1] << 8); ch = fb[2] | (fb[3] << 8);
            rate = fb[4] | (fb[5] << 8); bits = fb[14] | (fb[15] << 8);
            if (sz > 16) fseek(f, sz - 16 + (sz & 1), SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            len = sz;
            datapos = ftell(f); datalen = sz;
            break;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    printf("IBMPLAY: %s  %u Hz, %u-bit, %s, %lu bytes\n", name, rate, bits,
           ch == 2 ? "stereo" : "mono", len);
    if (fmt != 1 || (bits != 8 && bits != 16) || (ch != 1 && ch != 2)) {
        printf("only PCM 8/16-bit mono/stereo\n"); return 1;
    }
    for (i = 0; i < (int)(sizeof rtab / sizeof rtab[0]); i++)
        if (rtab[i].rate == rate) { found = 1; break; }
    if (!found) { printf("%u Hz is not in the codec rate table\n", rate); return 1; }
    dfr = (unsigned char)((rtab[i].dfs << 3) | (ch == 2 ? 4 : 0) | (bits == 8 ? 3 : 0));
    scr = (unsigned char)(0x86 | (rtab[i].xtal << 4));
    mbits = (unsigned char)((bits == 16 ? 0x20 : 0) | (ch == 2 ? 0x10 : 0));
    silence = bits == 8 ? 0x8080 : 0x0000;
    printf("  DFR %02X  SCR %02X  mode %02X  ports %03X/%03X  ahead %u words\n",
           dfr, scr, mbits, b0, b1, ahead);
    if (inpw(R_CMD) == 0xFFFF) { printf("card not decoding at %03X - run IBMAUDGO first\n", b1); return 1; }

    if (codec_setup()) { printf("codec did not answer the control handshake\n"); finish(); return 2; }
    prepare(silence);
    lastpos = 0; consumed = 0;
    start(silence);
    lastpos = inpw(R_POS) & (RING - 1);

    for (pass = 0; pass <= repeat && !aborted; pass++) {
    if (pass) {                          /* card stalled on an empty ring: */
        unsigned long w = ticks();       /* wait, then refill with no control */
        while (ticks() - w < 9) track();
        printf("  pass %u: position %04X before refill\n", pass, inpw(R_POS));
        fseek(f, datapos, SEEK_SET); len = datalen; padded = 0;
        t0 = 1;                          /* no hold release this time */
        c0 = consumed; t0 = ticks(); if (!t0) t0 = 1;
    } else { t0 = 0; c0 = 0; }
    while (len || consumed < dataend) {
        track();
        if (kbhit()) { getch(); aborted = 1; break; }
        while (len && written - consumed < ahead) {
            unsigned want = (unsigned)(ahead - (written - consumed));
            unsigned bytes, words;
            if (want > sizeof buf / 2) want = sizeof buf / 2;
            bytes = want * 2;
            if (bytes > len) bytes = (unsigned)len;
            bytes = (unsigned)fread(buf, 1, bytes, f);
            if (!bytes) { len = 0; break; }
            if (bytes & 1) ((unsigned char *)buf)[bytes++] = (unsigned char)silence;
            words = bytes / 2;
            outsw_near(R_FIFO, buf, words);
            written += words;
            len -= (bytes > len) ? len : bytes;
        }
        if (!len && !padded) {           /* pad the tail with silence, once */
            dataend = written;
            for (i = 0; i < 1024; i++) outpw(R_FIFO, silence);
            written += 1024;
            padded = 1;
        }
        if (!t0) {                       /* ring primed: release the hold */
            go();
            t0 = ticks(); c0 = consumed;
            if (!t0) t0 = 1;
        }
        if (t0 && ticks() - t0 > 18L * 20) { printf("  timeout\n"); aborted = 1; break; }
    }
    t1 = ticks();
    if (t1 > t0) {
        unsigned long wps = (consumed - c0) * 182L / ((t1 - t0) * 10L);
        printf("  pass %u: consumed %lu words in %lu ticks = %lu words/s\n",
               pass, consumed - c0, t1 - t0, wps);
    }
    }
    finish();
    fclose(f);

    printf("  position %04X, consumed %lu of %lu words written\n",
           inpw(R_POS), consumed, written);
    if (t1 > t0) {
        unsigned long wps = (consumed - c0) * 182L / ((t1 - t0) * 10L);
        unsigned long fps = (bits == 16 ? wps : wps * 2) / ch;
        printf("  played %lu words in %lu ticks: %lu words/s = %lu frames/s (file %u)%s\n",
               consumed - c0, t1 - t0, wps, fps, rate, aborted ? "  [key]" : "");
    }
    return 0;
}
