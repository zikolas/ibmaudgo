/* IBMSNAP.C - read-only snapshot of the IBM PCMCIA Audio Adapter as the
 * VENDOR driver leaves it.
 *
 * The vendor's AUDIODOC.TXT says the card "requires the exclusive use of the
 * memory from 250-25F and 340-347" - so the CIS range 0 (0x201-0x336) is a
 * hull, and the real decode is those two blocks.  Our own bring-up gets the
 * card to claim the bus but leaves the register file dead: 0x258/0x259 accept
 * and return any byte without ever indexing anything, which is what an AD1848
 * looks like while it is held in reset.
 *
 * So load PCAUDDD.SYS, let it configure the card, and read the result back.
 * This tool WRITES NOTHING to the card.  It dumps the PCIC socket registers
 * (so we learn the window layout, interface mode and IRQ the vendor driver
 * chose), the COR/CCSR from attribute memory through a borrowed and restored
 * memory window, and both device blocks through whatever windows are already
 * mapped - plain inp(), no window programming.
 *
 *   IBMSNAP        one snapshot
 *   IBMSNAP /W     watch 0x250-0x25F and 0x340-0x347, printing only when a
 *                  byte changes - run this while the vendor driver plays a
 *                  file to catch the registers moving.
 *   IBMSNAP /O     open the driver's AUDIO1$ device first, so PCAUDDD.SYS
 *                  configures the card, and hold it open for the whole run.
 *                  That gives an in-process look at the fully working card
 *                  instead of reading it over the serial link while a
 *                  full-screen vendor program owns the machine.
 *   IBMSNAP /E     bring the card up OURSELVES using the exact PCIC register
 *                  values the vendor stack was observed to use - no driver,
 *                  no Card Services.  The observed configuration is:
 *                    power   0xF0  (card power + auto + OE, Vpp OFF)
 *                    intctl  0xEA  (I/O card, reset released, IRQ 10)
 *                    ioctl   0xB2  (win0 IOCS16 from card;
 *                                   win1 16-BIT DATA + IOCS16 + wait states)
 *                    winen   0xE1  (mem win0 + io win0 + io win1)
 *                    io win0 0x250-0x25F, io win1 0x340-0x347
 *                    COR     0x41  (config index 1 + level-mode IREQ)
 *                    attr window: 4K at 0xCC000 mapping card attr 0xF000
 *                  Our own earlier bring-up differed mainly in ioctl - it used
 *                  8-bit cycles for both windows, and the 0x340 block only
 *                  ever answered a byte read with zeros.
 *   IBMSNAP /P     POKE TEST - the one experiment that can invalidate the
 *                  whole backend plan, so it is worth running first.  While
 *                  the vendor driver is playing, hammer a square wave into
 *                  the WORD at window1+6 (0xF46 at the alias the Windows
 *                  driver picks).  That port is the only one that moves
 *                  during playback and freezes in silence, which is why it
 *                  is believed to be the 16-bit PCM data register - the
 *                  SCP-55's R3-at-base+7 shape.  If that is right the
 *                  playing audio audibly breaks up; if the sound is
 *                  untouched the port is a status/counter and the reading
 *                  is wrong.  Either answer is worth two seconds.
 *   IBMSNAP /H     hunt for the codec: write an index to every port in both
 *                  blocks and look for the AD1848 Index Address Register,
 *                  which echoes bits 5:0.  Each byte is restored.  Any base
 *                  that echoes then gets the full identification.
 *
 * Build:  C:\WATCOM\BLD IBMSNAP
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>
#include <fcntl.h>
#include <io.h>

#define PCIC    0x3E0
#define ATTRSEG 0xD000

static unsigned pidx = PCIC, soff = 0;

/* PCIC index+data as one uninterruptible pair: if Card Services is resident
 * its poller shares this index register and will otherwise clobber it. */
static unsigned char rd(unsigned r)
{
    unsigned char v;
    _disable(); outp(pidx, soff + r); v = (unsigned char)inp(pidx + 1); _enable();
    return v;
}
static void wr(unsigned r, unsigned v)
{
    _disable(); outp(pidx, soff + r); outp(pidx + 1, v); _enable();
}
static void dly(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) dly((unsigned long)(x) * 1000UL)

/* The card answers four aliases (250/650/A50/E50 + 340/740/B40/F40) and
 * whoever configured it chose one - the vendor's Windows driver picks
 * E50/F40, the canonical Windows Sound System base, not the 250/340 this
 * used to assume.  So take the bases from the PCIC's own I/O window
 * registers and work wherever the card actually is. */
static unsigned w0s, w0e, w1s, w1e;

static void get_windows(void)
{
    w0s = (unsigned)rd(0x08) | ((unsigned)rd(0x09) << 8);
    w0e = (unsigned)rd(0x0A) | ((unsigned)rd(0x0B) << 8);
    w1s = (unsigned)rd(0x0C) | ((unsigned)rd(0x0D) << 8);
    w1e = (unsigned)rd(0x0E) | ((unsigned)rd(0x0F) << 8);
    if (w0e < w0s || w0e - w0s > 0x3F) { w0s = 0x250; w0e = 0x25F; }
    if (w1e < w1s || w1e - w1s > 0x3F) { w1s = 0x340; w1e = 0x347; }
}

static void ports(void)
{
    unsigned a;
    printf("  %03X-%03X:", w0s, w0e);
    for (a = w0s; a <= w0e; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\n  %03X-%03X:", w1s, w1e);
    for (a = w1s; a <= w1e; a++) printf(" %02X", (unsigned char)inp(a));
    /* Window 1 is a WORD-width block: the vendor driver only ever touches
     * 34x as 16-bit accesses, which is also what the PCIC ioctl=B2h we read
     * off the running stack asks for (win1 = 16-bit data + wait states).
     * Byte probing it - which is all this tool did until now - is the wrong
     * cycle and is the likely reason nothing there ever echoed an index. */
    printf("\n  %03X-%03X as WORDS:", w1s, w1e);
    for (a = w1s; a < w1e; a += 2) printf(" %04X", (unsigned)inpw(a));
    printf("\n");
}


/* Bring the card up with the vendor-observed register values.  The I/O ranges
 * are checked against the link and the PCIC first - a window over either is a
 * physical power-cycle to recover, so it is refused rather than programmed. */
static int io_range_unsafe(unsigned s, unsigned e)
{
    if (s <= 0x3FF && e >= 0x3F8) return 1;          /* COM1 - the link      */
    if (s <= 0x3E7 && e >= 0x3E0) return 1;          /* PCIC index/data      */
    return 0;
}

static int enable_card(void)
{
    unsigned char __far *cor;
    int i;

    if (io_range_unsafe(0x250, 0x25F) || io_range_unsafe(0x340, 0x347)) {
        printf("REFUSED: window would span the link or the PCIC\n");
        return 0;
    }
    if ((rd(0x01) & 0x0C) != 0x0C) { printf("no card in socket\n"); return 0; }

    wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00); MS(300);   /* clean slate */
    wr(0x02, 0xF0); MS(50);
    wr(0x03, 0x40); MS(20);                       /* memory mode to reach COR */

    /* 4K attribute window at 0xCC000 onto card attribute page 0x0F, so the
     * COR at 0xFFF0 lands at CC00:0FF0 - the mapping the vendor stack used. */
    wr(0x10, 0xCC); wr(0x11, 0x00);
    wr(0x12, 0xCC); wr(0x13, 0x00);
    wr(0x14, 0x43); wr(0x15, 0x7F);               /* offset 0x3F43 | REG#     */
    wr(0x06, 0x01); MS(20);

    for (i = 0; i < 250; i++) {
        if (*(unsigned char __far *)MK_FP(0xCC00, 0x0000) != 0xFF) break;
        MS(20);
    }

    cor = (unsigned char __far *)MK_FP(0xCC00, 0x0FF0);
    *cor = 0x41; MS(50);
    printf("COR <- 41, reads %02X   CCSR reads %02X\n",
           (unsigned char)*cor, (unsigned char)*(cor + 2));

    wr(0x08, 0x50); wr(0x09, 0x02);               /* io win0 0250-025F        */
    wr(0x0A, 0x5F); wr(0x0B, 0x02);
    wr(0x0C, 0x40); wr(0x0D, 0x03);               /* io win1 0340-0347        */
    wr(0x0E, 0x47); wr(0x0F, 0x03);
    wr(0x07, 0xB2);                               /* THE difference: 16-bit   */
    wr(0x03, 0xEA);                               /* I/O card, IRQ 10         */
    wr(0x06, 0xE1); MS(50);
    printf("enabled: power=%02X intctl=%02X winen=%02X ioctl=%02X\n",
           rd(0x02), rd(0x03), rd(0x06), rd(0x07));
    return 1;
}

/* ---- AD1848 / CS4231 identification ---------------------------------- */
static int cwait(unsigned cb)
{
    unsigned long i;
    for (i = 0; i < 200000UL; i++) if (!(inp(cb) & 0x80)) return 1;
    return 0;
}
static unsigned char cread(unsigned cb, unsigned char idx)
{
    cwait(cb); outp(cb, idx); return (unsigned char)inp(cb + 1);
}
static void cwrite(unsigned cb, unsigned char idx, unsigned char val)
{
    cwait(cb); outp(cb, idx); outp(cb + 1, val);
}

static void identify(unsigned cb)
{
    unsigned char i12, i25, a, b;
    int i, distinct = 0;
    unsigned char r[16];

    printf("\n--- identify at %03X ---\n", cb);
    printf("INIT %s  status(%03X)=%02X\n", cwait(cb) ? "clear" : "STUCK",
           cb + 2, (unsigned char)inp(cb + 2));

    for (i = 0; i < 16; i++) r[i] = cread(cb, (unsigned char)i);
    printf("mode1 I0-15 :");
    for (i = 0; i < 16; i++) printf(" %02X", r[i]);
    printf("\n");
    /* a real register file has DIFFERENT values per index; a dumb latch
       returns the last byte written no matter which index is selected. */
    for (i = 1; i < 16; i++) if (r[i] != r[0]) distinct = 1;
    if (!distinct) {
        printf("all 16 indices identical -> a latch, NOT a register file\n");
        return;
    }
    printf("indices differ -> real indexed register file\n");

    a = cread(cb, 0x0C);
    printf("I12=%02X %s\n", a, ((a & 0x8F) == 0x8A) ? "<== CS4248/AD1848 signature" : "");
    cwrite(cb, 0x00, 0xAA); b = cread(cb, 0x00);
    cwrite(cb, 0x00, 0x55); i12 = cread(cb, 0x00);
    printf("I0 scratch AA->%02X 55->%02X\n", b, i12);
    i12 = cread(cb, 0x0C);
    cwait(cb); outp(cb, (unsigned char)(0x40 | 0x0C)); outp(cb + 1, (unsigned char)(i12 | 0x40));
    cwait(cb); outp(cb, 0x0C);
    if (cread(cb, 0x0C) & 0x40) {
        i25 = cread(cb, 0x19);
        printf("MODE2 ok, I25 version=%02X : %s\n", i25,
               ((i25 & 0xE0) == 0x80) ? "CS4231" :
               ((i25 & 0xE0) == 0xA0) ? "CS4231A" : "unknown mode-2 part");
    } else printf("MODE2 refused -> mode-1-only (CS4248 / AD1848 class)\n");
}

/* Two stages, because an index register that echoes proves nothing on this
 * card - 0x258/0x259 and 0xE58/0xE59 both echo and both turned out to be
 * plain latches.  Stage 1 finds ports that echo an index.  Stage 2 is the
 * one that matters: pair each with every other port in the window and
 * demand that TWO DIFFERENT INDICES RETURN DIFFERENT DATA.  A latch cannot
 * do that; a real indexed register file cannot avoid it. */
/* CS4248-targeted sweep.
 *
 * The lead: the analog part may be a Crystal CS4248 - the same codec as
 * the ThinkPad 755C's internal audio.  What we know about that part from
 * that bench is what makes this testable:
 *   - I12 reads 0x8A.  That is the signature, and it is the ONE value worth
 *     hunting for; nothing else on this card would produce it by accident.
 *   - it is mode-1 only (I0-I15), so I25 should NOT answer like a CS4231.
 *   - MCE comes up SET and INIT (IAR bit 7) is set while the chip is busy;
 *     WHILE INIT IS SET THE CHIP IGNORES WRITES.  A disabled or busy part
 *     reads 0x80, not 0xFF.
 *
 * That last point is why the previous hunt could have missed it: it only
 * tried data ports behind index ports that ECHOED, and a CS4248 with INIT
 * stuck never echoes.  So try EVERY port as an index register regardless of
 * whether it echoes, wait for INIT to clear before each read, and flag 0x8A
 * wherever it appears.  Ports that merely mirror the index (straight or
 * inverted - 0xE5E does the inverted trick) are rejected.
 */
static void idx_wait(unsigned i)
{
    int k;
    for (k = 0; k < 20000; k++) if (!(inp(i) & 0x80)) return;
}

static unsigned char idx_read(unsigned i, unsigned d, unsigned char n)
{
    idx_wait(i);
    outp(i, n);
    idx_wait(i);
    return (unsigned char)inp(d);
}

/* Word-width variant of the hunt, for the 34x/F4x block. */
static void hunt_w(unsigned s, unsigned e)
{
    unsigned i, d;
    int hits = 0;
    for (i = s; i < e; i += 2) {
        unsigned save = (unsigned)inpw(i);
        for (d = s; d < e; d += 2) {
            unsigned v0, v12, v25;
            if (d == i) continue;
            outpw(i, 0x0000); v0  = (unsigned)inpw(d);
            outpw(i, 0x000C); v12 = (unsigned)inpw(d);
            outpw(i, 0x0019); v25 = (unsigned)inpw(d);
            if (v0 == v12 && v12 == v25) continue;
            if (v0 == 0x0000 && v12 == 0x000C && v25 == 0x0019) continue;
            printf("  WORD idx %03X data %03X: I0=%04X I12=%04X I25=%04X%s\n",
                   i, d, v0, v12, v25,
                   ((v12 & 0xFF) == 0x8A) ? "  <== I12=8A CS4248 SIGNATURE" : "");
            hits++;
        }
        outpw(i, save);
    }
    if (!hits) printf("  %03X-%03X: no WORD index/data pair behaves like a register file\n", s, e);
}

/* Square-wave poke into the suspected PCM data word. */
static void poke_test(void)
{
    unsigned long __far *bios = (unsigned long __far *)MK_FP(0x0040, 0x006C);
    unsigned long t0;
    unsigned port = w1s + 6;
    int flip = 0;
    long writes = 0;

    printf("\nPOKE TEST: hammering a square wave into WORD %03X for ~2s.\n", port);
    printf("  before: %03X=%04X  ctrl %03X=%04X  E5B=%02X E5C=%02X\n",
           port, (unsigned)inpw(port), w1s + 2, (unsigned)inpw(w1s + 2),
           (unsigned char)inp(w0s + 0x0B), (unsigned char)inp(w0s + 0x0C));
    printf("  LISTEN NOW - does the playing audio break up?\n");

    t0 = *bios;
    while (*bios - t0 < 36) {              /* ~2 s of BIOS ticks */
        int k;
        for (k = 0; k < 8; k++) { outpw(port, flip ? 0x4000 : 0xC000); writes++; }
        flip = !flip;
    }

    printf("  after:  %03X=%04X  ctrl %03X=%04X  E5B=%02X E5C=%02X\n",
           port, (unsigned)inpw(port), w1s + 2, (unsigned)inpw(w1s + 2),
           (unsigned char)inp(w0s + 0x0B), (unsigned char)inp(w0s + 0x0C));
    printf("  %ld words written.\n", writes);
}

static void hunt(unsigned s, unsigned e)
{
    unsigned i, d;
    int hits = 0;

    for (i = s; i <= e; i++) {
        unsigned char save = (unsigned char)inp(i);
        for (d = s; d <= e; d++) {
            unsigned char v0, v12, v25;
            if (d == i) continue;
            v0  = idx_read(i, d, 0x00);
            v12 = idx_read(i, d, 0x0C);
            v25 = idx_read(i, d, 0x19);
            if (v0 == v12 && v12 == v25) continue;             /* latch      */
            if (v0 == 0x00 && v12 == 0x0C && v25 == 0x19) continue; /* mirror */
            if (v0 == 0xFF && v12 == 0xF3 && v25 == 0xE6) continue; /* ~mirror*/
            printf("  idx %03X data %03X: I0=%02X I12=%02X I25=%02X%s%s\n",
                   i, d, v0, v12, v25,
                   (v12 == 0x8A) ? "  <== I12=8A CS4248 SIGNATURE" : "",
                   ((v25 & 0xE0) == 0xA0 || (v25 & 0xE0) == 0x80) ? "  (I25 mode2?)" : "");
            hits++;
        }
        outp(i, save);
    }
    if (!hits) printf("  %03X-%03X: no index/data pair behaves like a register file\n", s, e);
}

int main(int argc, char **argv)
{
    unsigned char r[0x40], en, i3;
    unsigned char sv[6];
    unsigned start, stop, woff;
    int i, wn = -1, watch = 0, doopen = 0, dohunt = 0, doenable = 0, dopoke = 0, h = -1;

    for (i = 1; i < argc; i++) {
        char c = argv[i][1];
        if (c == 'W' || c == 'w') watch = 1;
        if (c == 'O' || c == 'o') doopen = 1;
        if (c == 'H' || c == 'h') dohunt = 1;
        if (c == 'E' || c == 'e') doenable = 1;
        if (c == 'P' || c == 'p') dopoke = 1;
        if (c == 'S' || c == 's') { if (argv[i][2] == '1') soff = 0x40; }
    }

    if (doenable && !enable_card()) return 1;

    if (doopen) {
        h = open("AUDIO1$", O_RDWR | O_BINARY);
        if (h < 0) h = open("AUDIO2$", O_RDWR | O_BINARY);
        printf("open AUDIO1$/AUDIO2$ -> handle %d %s\n", h,
               (h < 0) ? "(FAILED - driver not loaded?)" : "(card configured by driver)");
        MS(200);
    }

    for (i = 0; i < 0x40; i++) r[i] = rd(i);
    get_windows();

    printf("IBMSNAP - vendor-configured card readback\n");
    printf("PCIC IDREV=%02X status=%02X power=%02X intctl=%02X winen=%02X ioctl=%02X\n",
           r[0x00], r[0x01], r[0x02], r[0x03], r[0x06], r[0x07]);
    i3 = r[0x03];
    printf("  interface: %s, reset %s, IRQ %u\n",
           (i3 & 0x20) ? "I/O card" : "memory card",
           (i3 & 0x40) ? "released" : "ASSERTED", (unsigned)(i3 & 0x0F));
    en = r[0x06];
    printf("  windows enabled: mem%s%s%s%s%s  io%s%s\n",
           (en & 0x01) ? " 0" : "", (en & 0x02) ? " 1" : "",
           (en & 0x04) ? " 2" : "", (en & 0x08) ? " 3" : "",
           (en & 0x10) ? " 4" : "",
           (en & 0x40) ? " 0" : "", (en & 0x80) ? " 1" : "");
    printf("  io win0 %04X-%04X   io win1 %04X-%04X\n",
           (unsigned)r[0x08] | ((unsigned)r[0x09] << 8),
           (unsigned)r[0x0A] | ((unsigned)r[0x0B] << 8),
           (unsigned)r[0x0C] | ((unsigned)r[0x0D] << 8),
           (unsigned)r[0x0E] | ((unsigned)r[0x0F] << 8));
    for (i = 0; i < 5; i++) {
        unsigned b = 0x10 + i * 8;
        if (!(en & (1 << i))) continue;
        printf("  mem win%d %04X-%04X off %04X%s\n", i,
               ((unsigned)r[b] | ((unsigned)r[b+1] << 8)) << 4,
               ((unsigned)r[b+2] | ((unsigned)r[b+3] << 8)) << 4,
               (unsigned)r[b+4] | ((unsigned)r[b+5] << 8),
               (r[b+5] & 0x40) ? " ATTR" : " common");
    }

    printf("\nPCIC regs 00-3F:");
    for (i = 0; i < 0x40; i++) {
        if ((i & 15) == 0) printf("\n  %02X:", i);
        printf(" %02X", r[i]);
    }
    printf("\n");

    /* borrow a disabled memory window for the COR, then put it back exactly */
    for (i = 0; i < 5; i++) if (!(en & (1 << i))) { wn = i; break; }
    if (wn < 0) printf("\nno free memory window - COR not read\n");
    else {
        unsigned b = 0x10 + wn * 8;
        for (i = 0; i < 6; i++) sv[i] = rd(b + i);
        start = ATTRSEG >> 8; stop = (ATTRSEG >> 8) + 15;
        woff  = ((unsigned)(0 - (ATTRSEG >> 8)) & 0x3FFF) | 0x4000;
        wr(b + 0, start & 0xFF); wr(b + 1, (start >> 8) & 0x3F);
        wr(b + 2, stop  & 0xFF); wr(b + 3, (stop  >> 8) & 0x3F);
        wr(b + 4, woff  & 0xFF); wr(b + 5, (woff  >> 8) & 0xFF);
        wr(0x06, (unsigned char)(en | (1 << wn))); MS(5);
        printf("\nCOR@FFF0=%02X  CCSR@FFF2=%02X  (via borrowed mem win%d)\n",
               (unsigned char)*(unsigned char __far *)MK_FP(ATTRSEG, 0xFFF0),
               (unsigned char)*(unsigned char __far *)MK_FP(ATTRSEG, 0xFFF2), wn);
        wr(0x06, en);
        for (i = 0; i < 6; i++) wr(b + i, sv[i]);
    }

    printf("\ndevice ports (as the vendor driver left them):\n");
    ports();

    if (watch) {
        unsigned char last[64], now[64];
        unsigned a;
        long n = 0;
        int nw = 0;
        printf("\nwatching %03X-%03X and %03X-%03X - play something now,"
               " any key to stop\n", w0s, w0e, w1s, w1e);
        for (i = 0, a = w0s; a <= w0e; a++, i++) last[i] = (unsigned char)inp(a);
        for (a = w1s; a <= w1e; a++, i++) last[i] = (unsigned char)inp(a);
        nw = i;
        while (!kbhit()) {
            int ch = 0;
            for (i = 0, a = w0s; a <= w0e; a++, i++) now[i] = (unsigned char)inp(a);
            for (a = w1s; a <= w1e; a++, i++) now[i] = (unsigned char)inp(a);
            for (i = 0; i < nw; i++) if (now[i] != last[i]) ch = 1;
            if (ch && n < 40) {
                printf("%6ld:", n);
                for (i = 0; i < nw; i++)
                    printf("%s%02X", (now[i] != last[i]) ? "*" : " ", now[i]);
                printf("\n");
                n++;
            }
            for (i = 0; i < nw; i++) last[i] = now[i];
        }
        getch();
    }

    if (dopoke) poke_test();

    if (dohunt) {
        printf("\n== codec hunt, window 0 (%03X-%03X) ==\n", w0s, w0e);
        hunt(w0s, w0e);
        printf("\n== codec hunt, window 1 (%03X-%03X) BYTE ==\n", w1s, w1e);
        hunt(w1s, w1e);
        printf("\n== codec hunt, window 1 (%03X-%03X) WORD ==\n", w1s, w1e);
        hunt_w(w1s, w1e);
        printf("\nports after hunt:\n");
        ports();
    }

    if (h >= 0) close(h);
    return 0;
}
