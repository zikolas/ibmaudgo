/* IBM16TR4.C - IBM NON-DSP AUDIO, take four: whose register is it, and is the
 * card's clock running?
 *
 * TR3 swept the whole declared map with the card configured and found only
 * four non-zero ports - 0x25A=A0, 0x25F=FF, 0x342=1A, and 0x335=28 which
 * appears ONLY once 0x342 bit 7 is set.  0x343=48 shows up only through a
 * 16-bit data path.  That is a gate, but two things are still unproven:
 *
 *   1. whether those ports are the CARD at all.  The PCIC window makes the
 *      host forward the cycle, but this ThinkPad may answer at 0x25A itself.
 *      Every port is therefore read twice - once with I/O window 0 DISABLED
 *      (pure host bus) and once ENABLED (card in the path).  Only a port that
 *      differs between the two is the card's.
 *   2. whether the card is actually running.  0x342 bit 4 changes state on its
 *      own; if it flips at a crystal-derived rate the silicon is clocked and
 *      alive, and only its register file is gated.  Timed against the BIOS
 *      tick over ~1s.
 *
 * Then an exhaustive index-register hunt with the gate held open: an AD1848 /
 * CS4248 Index Address Register echoes bits 5:0 of whatever is written, so
 * write 0x0A and 0x15 to every port in the two plausible blocks and look for
 * the echo.  Every byte is restored to what it was.
 *
 * Build:  C:\WATCOM\BLD IBM16TR4
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC    0x3E0
#define ATTRSEG 0xD000

static unsigned pidx = PCIC, soff = 0;
static unsigned char __far *cor;

static unsigned char rd(unsigned r){ outp(pidx, soff + r); return (unsigned char)inp(pidx + 1); }
static void          wr(unsigned r, unsigned v){ outp(pidx, soff + r); outp(pidx + 1, v); }
static void dly(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) dly((unsigned long)(x) * 1000UL)

static int io_range_unsafe(unsigned s, unsigned e)
{
    if (s <= 0x3FF && e >= 0x3F8) return 1;
    if (s <= 0x3E7 && e >= 0x3E0) return 1;
    return 0;
}

static void win0(unsigned s, unsigned e, unsigned char reg07)
{
    wr(0x06, rd(0x06) & ~0x40);
    wr(0x08, s & 0xFF); wr(0x09, (s >> 8) & 0xFF);
    wr(0x0A, e & 0xFF); wr(0x0B, (e >> 8) & 0xFF);
    wr(0x07, reg07);
    wr(0x06, rd(0x06) | 0x40);
    MS(2);
}
static void win0_off(void){ wr(0x06, rd(0x06) & ~0x40); MS(2); }

/* read one port with the window off (host) then on (card) */
static void hc(unsigned a)
{
    unsigned char h, c;
    win0_off();            h = (unsigned char)inp(a);
    win0(a & 0xFFE0, (a & 0xFFE0) + 0x1F, 0x00);
    c = (unsigned char)inp(a);
    printf("  %03X host=%02X card=%02X  %s\n", a, h, c,
           (h == c) ? "SAME -> host device or unclaimed" : "DIFFERS -> the card");
}

static unsigned long tick(void)
{
    unsigned long __far *t = (unsigned long __far *)MK_FP(0x0040, 0x006C);
    return *t;
}

/* count bit-4 transitions on `a` for `ticks` BIOS ticks (18.2/s) */
static void toggle_rate(unsigned a, int ticks)
{
    unsigned long t0, samples = 0, trans = 0;
    unsigned char last, v;
    last = (unsigned char)(inp(a) & 0x10);
    t0 = tick();
    while (tick() == t0) ;                      /* align to a tick edge       */
    t0 = tick();
    while (tick() - t0 < (unsigned long)ticks) {
        v = (unsigned char)(inp(a) & 0x10);
        if (v != last) { trans++; last = v; }
        samples++;
    }
    printf("  %03X bit4: %lu transitions in %lu samples over %d ticks (~%lu/s)\n",
           a, trans, samples, ticks, (trans * 182UL) / (10UL * (unsigned long)ticks));
}

static void iar_hunt(unsigned s, unsigned e)
{
    unsigned a;
    int found = 0;
    win0(s & 0xFFE0, (e | 0x1F), 0x00);
    for (a = s; a <= e; a++) {
        unsigned char orig = (unsigned char)inp(a), r1, r2;
        outp(a, 0x0A); r1 = (unsigned char)inp(a);
        outp(a, 0x15); r2 = (unsigned char)inp(a);
        outp(a, orig);
        if ((r1 & 0x3F) == 0x0A && (r2 & 0x3F) == 0x15) {
            printf("  %03X ECHOES: 0A->%02X 15->%02X  <== index register\n", a, r1, r2);
            found++;
        } else if (r1 != r2) {
            printf("  %03X stores: 0A->%02X 15->%02X\n", a, r1, r2);
            found++;
        }
    }
    if (!found) printf("  %03X-%03X: no register echoes or stores\n", s, e);
}

int main(int argc, char **argv)
{
    unsigned char idrev, ifs;
    unsigned start, stop, woff;
    int i;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;

    printf("IBM16TR4 - host/card attribution + clock check\n");
    idrev = rd(0x00); ifs = rd(0x01);
    if ((idrev & 0xC0) != 0x80) { printf("not an 82365 - abort\n"); return 1; }
    if ((ifs & 0x0C) != 0x0C)   { printf("no card - abort\n"); return 1; }
    if (io_range_unsafe(0x200, 0x347)) { printf("refused\n"); return 1; }

    wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00); MS(200);
    wr(0x02, 0x95); MS(20);
    wr(0x03, 0x40); MS(10);

    start = ATTRSEG >> 8; stop = (ATTRSEG >> 8) + 15;
    woff  = ((unsigned)(0 - (ATTRSEG >> 8)) & 0x3FFF) | 0x4000;
    wr(0x10, start & 0xFF); wr(0x11, (start >> 8) & 0x3F);
    wr(0x12, stop  & 0xFF); wr(0x13, (stop  >> 8) & 0x3F);
    wr(0x14, woff  & 0xFF); wr(0x15, (woff  >> 8) & 0xFF);
    wr(0x06, 0x01); MS(20);
    for (i = 0; i < 250; i++) { if (*(unsigned char __far *)MK_FP(ATTRSEG,0) != 0xFF) break; MS(20); }

    cor = (unsigned char __far *)MK_FP(ATTRSEG, 0xFFF0);
    wr(0x03, 0x60);
    *cor = 0x01; MS(50);
    printf("COR=%02X CCSR=%02X\n\n", (unsigned char)*cor, (unsigned char)*(cor + 2));

    printf("== who owns these ports? (gate CLOSED) ==\n");
    hc(0x25A); hc(0x25F); hc(0x335); hc(0x342); hc(0x343);

    printf("\n== is the card clocked? (gate CLOSED) ==\n");
    win0(0x340, 0x347, 0x00);
    toggle_rate(0x342, 18);

    printf("\n== open the gate: 342 |= 80 ==\n");
    printf("  342 %02X -> ", (unsigned char)inp(0x342));
    outp(0x342, (unsigned char)(inp(0x342) | 0x80)); MS(50);
    printf("%02X\n", (unsigned char)inp(0x342));
    toggle_rate(0x342, 18);

    printf("\n== who owns these ports? (gate OPEN) ==\n");
    hc(0x25A); hc(0x25F); hc(0x335); hc(0x342); hc(0x343);

    printf("\n== index-register hunt, gate OPEN ==\n");
    win0(0x340, 0x347, 0x00);
    outp(0x342, (unsigned char)(inp(0x342) | 0x80));
    iar_hunt(0x330, 0x347);
    win0(0x340, 0x347, 0x00);
    outp(0x342, (unsigned char)(inp(0x342) | 0x80));
    iar_hunt(0x220, 0x22F);
    win0(0x340, 0x347, 0x00);
    outp(0x342, (unsigned char)(inp(0x342) | 0x80));
    iar_hunt(0x250, 0x25F);

    printf("\ndone - card left enabled, gate open\n");
    return 0;
}
