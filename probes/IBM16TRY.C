/* IBM16TRY.C - bring-up probe for the IBM "NON-DSP AUDIO" PC Card.
 *
 *   VERS_1  "IBM | NON-DSP AUDIO | 0933967 | NONE"
 *   MANFID  00A4 / 0022
 *   COR     attribute 0xFFF0 (CCSR 0xFFF2), single config, index 1, I/O iface
 *   I/O     10 address lines (fixed map, aliases every 0x400)
 *             range 0  0x201-0x336   range 1  0x340-0x346
 *   IRQ     any 0-15, level capable.  No memory windows, no 0x388 in the CIS.
 *
 * One-shot on purpose: enable + configure + probe must not be split across
 * host round-trips, and we never poke the PCIC from the other side of the
 * serial link (the Card Services poller clobbers the index register).
 *
 * What it does: power the socket, gate on the CIS with the two-clause data
 * gate, confirm the MANFID, write the COR, map two tight I/O windows, then
 * hunt for an AD1848/CS4248-class codec IAR across every plausible base and
 * fall back to SB-DSP and MPU-401 falsifiers.
 *
 * Read-mostly: the only write to the card is the COR (config index 1, exactly
 * what the CIS declares) plus codec index-register probing.  No blind sweeps.
 *
 * Build (Open Watcom, 16-bit real mode, small model):  C:\WATCOM\BLD IBM16TRY
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>
#include <string.h>

#define PCIC     0x3E0
#define WANT_MFC 0x00A4
#define WANT_CRD 0x0022
#define ATTRSEG  0xD000         /* CONFIG.SYS keeps Jemm off D000-DFFF        */

static unsigned pidx = PCIC, soff = 0;

static unsigned char rd(unsigned r){ outp(pidx, soff + r); return (unsigned char)inp(pidx + 1); }
static void          wr(unsigned r, unsigned v){ outp(pidx, soff + r); outp(pidx + 1, v); }
static void dly(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) dly((unsigned long)(x) * 1000UL)

/* ---- house rule: an I/O window must never span the COMrade link or the
 * PCIC's own index/data pair.  Recovery from either is a physical power
 * cycle, and the box reports connected the whole time it is dead. ---------- */
static int io_range_unsafe(unsigned start, unsigned stop)
{
    if (start <= 0x3FF && stop >= 0x3F8) return 1;      /* COM1  0x3F8-0x3FF  */
    if (start <= 0x3E7 && stop >= 0x3E0) return 1;      /* PCIC  0x3E0-0x3E7  */
    return 0;
}

/* ---- CIS walk (attribute bytes live at 2x host spacing) ------------------ */
static unsigned g_manf, g_card, g_cfg;
static char     g_vers[80];

static void read_cis(unsigned seg)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(seg, 0);
    unsigned off = 0;
    int g, vi = 0, m;

    g_manf = g_card = 0; g_cfg = 0; g_vers[0] = 0;
    for (g = 0; g < 64; g++) {
        unsigned char code = p[off], link;
        if (code == 0xFF) break;
        if (code == 0x00) { off += 2; continue; }
        link = p[off + 2];
        if (code == 0x20) {                             /* CISTPL_MANFID      */
            g_manf = (unsigned)p[off + 4] | ((unsigned)p[off + 6] << 8);
            g_card = (unsigned)p[off + 8] | ((unsigned)p[off + 10] << 8);
        } else if (code == 0x1A) {                      /* CISTPL_CONFIG      */
            int rasz = (p[off + 4] & 0x03) + 1;
            g_cfg = p[off + 8];
            if (rasz >= 2) g_cfg |= (unsigned)p[off + 10] << 8;
        } else if (code == 0x15) {                      /* CISTPL_VERS_1      */
            for (m = 2; m < link; m++) {
                unsigned char c = p[off + 4 + 2 * m];
                if (vi < 79) g_vers[vi++] = c ? (char)c : ' ';
            }
            g_vers[vi] = 0;
        }
        if (link == 0xFF) break;
        off += ((unsigned)link + 2) * 2;
        if (off >= 0x3000) break;
    }
    while (vi > 0 && g_vers[vi - 1] == ' ') g_vers[--vi] = 0;
}

/* Two-clause data gate: byte 0 non-FF AND four dense bytes stable across two
 * reads 20ms apart, ~5s cap.  An all-FF CIS almost always means the read came
 * too soon after socket power, not a blank card. */
static int cis_settle(unsigned seg)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(seg, 0);
    unsigned char a[4], b[4];
    int t, k;

    for (k = 0; k < 4; k++) a[k] = p[k * 2];
    for (t = 0; t < 250; t++) {
        MS(20);
        for (k = 0; k < 4; k++) b[k] = p[k * 2];
        for (k = 0; k < 4 && a[k] == b[k]; k++) ;
        if (k == 4 && b[0] != 0xFF) break;
        for (k = 0; k < 4; k++) a[k] = b[k];
    }
    printf("CIS settled after %d polls (%d ms)\n", t, t * 20);
    return p[0] != 0xFF;
}

/* ---- AD1848 / CS4248 / CS4231 codec ------------------------------------- */
static int cwait(unsigned cb)
{
    unsigned long i;
    for (i = 0; i < 60000UL; i++) if (!(inp(cb) & 0x80)) return 1;
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
static void cwrite_mce(unsigned cb, unsigned char idx, unsigned char val)
{
    cwait(cb); outp(cb, (unsigned char)(0x40 | idx)); outp(cb + 1, val);
    cwait(cb); outp(cb, idx);
}

/* Cheap discriminator: the Index Address Register echoes bits 5:0 of whatever
 * you wrote.  A float reads FF, a dead decode reads 00 or the last value. */
static int iar_live(unsigned cb)
{
    unsigned char r1, r2;
    outp(cb, 0x0A); r1 = (unsigned char)inp(cb);
    outp(cb, 0x15); r2 = (unsigned char)inp(cb);
    printf("  base %03X: IAR 0A->%02X 15->%02X stat=%02X pio=%02X",
           cb, r1, r2, (unsigned char)inp(cb + 2), (unsigned char)inp(cb + 3));
    if ((r1 & 0x3F) == 0x0A && (r2 & 0x3F) == 0x15) { printf("  <== LIVE\n"); return 1; }
    printf("\n");
    return 0;
}

static void codec_id(unsigned cb)
{
    unsigned char i12, i25, t1, t2;
    int i;

    printf("\n--- codec at %03X ---\n", cb);
    printf("INIT %s\n", cwait(cb) ? "clear" : "STUCK (never released)");

    cwrite(cb, 0x00, 0xAA); t1 = cread(cb, 0x00);
    cwrite(cb, 0x00, 0x55); t2 = cread(cb, 0x00);
    printf("I0 scratch: wrAA->%02X wr55->%02X %s\n", t1, t2,
           (t1 != t2) ? "(register file responds)" : "(DEAD - no storage)");

    printf("mode1 I0-15 :");
    for (i = 0; i < 16; i++) printf(" %02X", cread(cb, (unsigned char)i));
    printf("\n");

    i12 = cread(cb, 0x0C);
    printf("I12=%02X ", i12);
    if ((i12 & 0x8F) == 0x8A) printf("(0x8A = CS4248/AD1848 signature) ");
    cwrite_mce(cb, 0x0C, (unsigned char)(i12 | 0x40));
    i12 = cread(cb, 0x0C);
    printf("-> MODE2 attempt -> I12=%02X\n", i12);

    if (i12 & 0x40) {
        printf("mode2 I16-31:");
        for (i = 16; i < 32; i++) printf(" %02X", cread(cb, (unsigned char)i));
        printf("\n");
        i25 = cread(cb, 0x19);
        printf("I25 version=%02X : ", i25);
        switch (i25 & 0xE0) {
            case 0x80: printf("CS4231\n");  break;
            case 0xA0: printf("CS4231A\n"); break;
            default:   printf("unknown\n"); break;
        }
    } else {
        printf("MODE2 refused -> mode-1-only part (CS4248 / AD1848 class)\n");
    }
}

/* ---- falsifiers ---------------------------------------------------------- */
static void sb_try(unsigned base)
{
    int i;
    outp(base + 6, 1); MS(1); outp(base + 6, 0); MS(1);
    for (i = 0; i < 1000; i++)
        if (inp(base + 0x0E) & 0x80) {
            printf("SB DSP reset @%03X -> %02X %s\n", base,
                   (unsigned char)inp(base + 0x0A), "(AA = SB DSP PRESENT)");
            return;
        }
    printf("SB DSP reset @%03X -> no response (no SB DSP)\n", base);
}

static void mpu_try(unsigned base)
{
    int i;
    outp(base + 1, 0xFF);                       /* UART reset                 */
    for (i = 0; i < 20000; i++)
        if (!(inp(base + 1) & 0x80)) {
            printf("MPU-401 @%03X -> %02X (FE = ACK, UART present)\n", base,
                   (unsigned char)inp(base));
            return;
        }
    printf("MPU-401 @%03X -> no ACK (stat=%02X)\n", base,
           (unsigned char)inp(base + 1));
}

static void dump(unsigned start, unsigned stop, char *tag)
{
    unsigned a;
    printf("%s %03X-%03X:", tag, start, stop);
    for (a = start; a <= stop; a++) {
        if (((a - start) & 15) == 0 && a != start) printf("\n            %03X:", a);
        printf(" %02X", (unsigned char)inp(a));
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned char __far *cor;
    unsigned char idrev, ifs;
    unsigned start, stop, woff;
    unsigned w0s = 0x330, w0e = 0x34F, w1s = 0x220, w1e = 0x22F;
    static unsigned cand[] = { 0x340, 0x344, 0x330, 0x334, 0x338, 0x33C,
                               0x220, 0x224, 0x228, 0 };
    int i, nlive = 0;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;     /* socket 1           */

    printf("IBM16TRY - IBM NON-DSP AUDIO bring-up probe\n");

    idrev = rd(0x00);
    printf("PCIC IDREV=%02X ", idrev);
    if ((idrev & 0xC0) != 0x80) { printf("- not an 82365, abort\n"); return 1; }
    ifs = rd(0x01);
    printf("IntfStat=%02X\n", ifs);
    if ((ifs & 0x0C) != 0x0C) { printf("no card in socket - abort\n"); return 1; }

    wr(0x02, 0x95); MS(20);                             /* 5V on              */
    if (!(rd(0x01) & 0x40)) { printf("no power-up - abort\n"); wr(0x02, 0); return 1; }
    wr(0x03, 0x40); MS(10);                             /* reset off, mem mode*/

    /* 64KB attribute window: card 0x0000-0xFFFF at D000:0000, so both the CIS
     * at 0 and the COR at 0xFFF0 are reachable without reprogramming. */
    start = ATTRSEG >> 8; stop = (ATTRSEG >> 8) + 15;
    woff  = ((unsigned)(0 - (ATTRSEG >> 8)) & 0x3FFF) | 0x4000;   /* REG# = attr */
    wr(0x06, 0x00);                                     /* windows off first  */
    wr(0x10, start & 0xFF); wr(0x11, (start >> 8) & 0x3F);
    wr(0x12, stop  & 0xFF); wr(0x13, (stop  >> 8) & 0x3F);
    wr(0x14, woff  & 0xFF); wr(0x15, (woff  >> 8) & 0xFF);
    wr(0x06, 0x01);
    MS(20);

    if (!cis_settle(ATTRSEG)) {
        printf("CIS stayed all-FF past the gate - abort\n");
        wr(0x06, 0); wr(0x03, 0); wr(0x02, 0); return 1;
    }
    read_cis(ATTRSEG);
    printf("CIS \"%s\"\n  MANFID %04X/%04X  COR@%04X\n",
           g_vers, g_manf, g_card, g_cfg);
    if (g_manf != WANT_MFC || g_card != WANT_CRD) {
        printf("not the IBM NON-DSP AUDIO card (want %04X/%04X) - abort\n",
               WANT_MFC, WANT_CRD);
        wr(0x06, 0); wr(0x03, 0); wr(0x02, 0); return 1;
    }
    if (!g_cfg) g_cfg = 0xFFF0;

    if (io_range_unsafe(w0s, w0e) || io_range_unsafe(w1s, w1e)) {
        printf("REFUSED: window would span the link or the PCIC\n");
        wr(0x06, 0); wr(0x03, 0); wr(0x02, 0); return 1;
    }

    /* baseline: the card should not decode anything before the COR is written */
    printf("\n== before COR ==\n");
    dump(w1s, w1e, "io");
    dump(w0s, w0e, "io");

    cor = (unsigned char __far *)MK_FP(ATTRSEG, g_cfg);
    *cor = 0x01;                                        /* config index 1     */
    MS(10);
    printf("\nCOR@%04X <- 01, reads %02X   CCSR@%04X reads %02X\n",
           g_cfg, (unsigned char)*cor, g_cfg + 2,
           (unsigned char)*(cor + 2));

    /* I/O windows.  Reg 0x06 bit cleared before touching start/end - a window
     * half-reprogrammed in place has killed a link before. */
    wr(0x06, 0x01);
    wr(0x08, w0s & 0xFF); wr(0x09, (w0s >> 8) & 0xFF);
    wr(0x0A, w0e & 0xFF); wr(0x0B, (w0e >> 8) & 0xFF);
    wr(0x0C, w1s & 0xFF); wr(0x0D, (w1s >> 8) & 0xFF);
    wr(0x0E, w1e & 0xFF); wr(0x0F, (w1e >> 8) & 0xFF);
    wr(0x07, 0x00);                                     /* both windows 8-bit */
    wr(0x03, 0x60);                                     /* I/O card, no IRQ   */
    wr(0x06, 0xC1);                                     /* mem0 + IO0 + IO1   */
    MS(10);

    printf("\n== after COR ==\n");
    dump(w1s, w1e, "io");
    dump(w0s, w0e, "io");

    printf("\n== codec IAR hunt ==\n");
    for (i = 0; cand[i]; i++)
        if (iar_live(cand[i])) { nlive++; codec_id(cand[i]); }

    if (!nlive) {
        printf("\nno AD1848-class IAR found - running falsifiers\n");
        printf("\n== falsifiers ==\n");
        sb_try(0x220);
        mpu_try(0x330);
        mpu_try(0x340);
    }

    printf("\ndone - card LEFT ENABLED (COR=01, io %03X-%03X + %03X-%03X)\n",
           w0s, w0e, w1s, w1e);
    return 0;
}
