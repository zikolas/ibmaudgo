/* IBM16TR3.C - IBM NON-DSP AUDIO, take three: sweep the whole declared I/O map.
 *
 * What TR2 established:
 *   COR=00 -> every mapped port reads FF (card not driving)
 *   COR=01 -> every mapped port reads 00, INCLUDING 0x350-0x35F which the card
 *             never declares.  So a configured card drives the bus low for
 *             anything it does not implement, and 0x00 means "no register
 *             here", not "no card".
 *   The one exception in 0x340-0x347 is 0x342: reads 0A/1A (bit 4 toggles on
 *   its own), and bit 7 is read/write storage.
 *
 * So: walk the ENTIRE declared map in 32-byte windows and report every port
 * that is not 0x00.  Two passes per window catch self-changing bits.  Then
 * re-sweep after setting 0x342 bit 7, after steering an IRQ, and with the
 * window forced to a 16-bit data path - each a candidate for the gate that is
 * holding the rest of the card asleep.
 *
 * Windows stay 32 bytes, and 0x2F8-0x2FF (COM2/IR on this host) is skipped
 * even though the card's declared range 0 covers it.  0x3E0-0x3E7 and
 * 0x3F8-0x3FF are refused outright - recovery from either is a power cycle.
 *
 * Build:  C:\WATCOM\BLD IBM16TR3
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC    0x3E0
#define ATTRSEG 0xD000
#define LO      0x200
#define HI      0x347
#define NP      (HI - LO + 1)

static unsigned pidx = PCIC, soff = 0;
static unsigned char __far *cor;
static unsigned char base[NP], cur[NP];

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

/* one 32-byte segment of the sweep */
static struct { unsigned s, e; } seg[] = {
    { 0x200, 0x21F }, { 0x220, 0x23F }, { 0x240, 0x25F }, { 0x260, 0x27F },
    { 0x280, 0x29F }, { 0x2A0, 0x2BF }, { 0x2C0, 0x2DF }, { 0x2E0, 0x2F7 },
    { 0x300, 0x31F }, { 0x320, 0x33F }, { 0x340, 0x347 }, { 0, 0 }
};

/* Program I/O window 0 over [s,e] and read every port twice.  The enable bit
 * is cleared before the start/end registers are touched - a window rewritten
 * in place has spanned the link and killed a box before. */
static void sweep(unsigned char *out, unsigned char reg07)
{
    int i;
    unsigned a;
    for (i = 0; i < NP; i++) out[i] = 0;
    for (i = 0; seg[i].e; i++) {
        unsigned s = seg[i].s, e = seg[i].e;
        if (io_range_unsafe(s, e)) { printf("  [refused %03X-%03X]\n", s, e); continue; }
        wr(0x06, rd(0x06) & ~0x40);                   /* IO win0 off first    */
        wr(0x08, s & 0xFF); wr(0x09, (s >> 8) & 0xFF);
        wr(0x0A, e & 0xFF); wr(0x0B, (e >> 8) & 0xFF);
        wr(0x07, reg07);
        wr(0x06, rd(0x06) | 0x40);
        MS(2);
        for (a = s; a <= e; a++) out[a - LO] = (unsigned char)inp(a);
        MS(5);
        for (a = s; a <= e; a++) {                    /* 2nd pass: catch toggles */
            unsigned char v = (unsigned char)inp(a);
            if (v != out[a - LO]) {
                printf("  %03X toggles %02X/%02X\n", a, out[a - LO], v);
                out[a - LO] |= v;
            }
        }
    }
    wr(0x06, rd(0x06) & ~0x40);
}

static void show(char *tag, unsigned char *v)
{
    int i, n = 0;
    printf("%s live ports:", tag);
    for (i = 0; i < NP; i++) if (v[i]) { printf(" %03X=%02X", LO + i, v[i]); n++; }
    if (!n) printf(" (none)");
    printf("\n");
}

static void diff(char *tag, unsigned char *a, unsigned char *b)
{
    int i, n = 0;
    printf("%s changes:", tag);
    for (i = 0; i < NP; i++) if (a[i] != b[i]) { printf(" %03X %02X->%02X", LO + i, a[i], b[i]); n++; }
    if (!n) printf(" (none)");
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned char idrev, ifs;
    unsigned start, stop, woff;
    int i;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;

    printf("IBM16TR3 - full declared-map sweep\n");
    idrev = rd(0x00); ifs = rd(0x01);
    if ((idrev & 0xC0) != 0x80) { printf("not an 82365 - abort\n"); return 1; }
    if ((ifs & 0x0C) != 0x0C)   { printf("no card - abort\n"); return 1; }

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
    printf("COR=%02X CCSR=%02X\n", (unsigned char)*cor, (unsigned char)*(cor + 2));

    printf("\n== phase 1: as configured (8-bit window) ==\n");
    sweep(base, 0x00);
    show("p1", base);

    printf("\n== phase 2: 342 |= 80 ==\n");
    wr(0x06, rd(0x06) & ~0x40);
    wr(0x08, 0x40); wr(0x09, 0x03); wr(0x0A, 0x47); wr(0x0B, 0x03);
    wr(0x07, 0x00); wr(0x06, rd(0x06) | 0x40); MS(2);
    printf("342 was %02X, ", (unsigned char)inp(0x342));
    outp(0x342, (unsigned char)(inp(0x342) | 0x80)); MS(50);
    printf("now %02X\n", (unsigned char)inp(0x342));
    sweep(cur, 0x00);
    diff("p2", base, cur);

    printf("\n== phase 3: + IRQ 5 steered to the socket ==\n");
    wr(0x03, 0x65); MS(50);
    sweep(cur, 0x00);
    diff("p3", base, cur);

    printf("\n== phase 4: + 16-bit data path (reg07=01) ==\n");
    sweep(cur, 0x01);
    diff("p4", base, cur);

    printf("\n== phase 5: fresh COR after SRESET, 342 untouched ==\n");
    wr(0x03, 0x60);
    *cor = 0x80; MS(20); *cor = 0x00; MS(50); *cor = 0x41; MS(100);
    printf("COR<-41 reads %02X\n", (unsigned char)*cor);
    sweep(cur, 0x00);
    diff("p5", base, cur);

    wr(0x06, rd(0x06) | 0x40);
    printf("\ndone - card left enabled\n");
    return 0;
}
