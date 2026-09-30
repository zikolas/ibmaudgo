/* IBM16TR6.C - IBM NON-DSP AUDIO: complete read/write map of the declared I/O.
 *
 * Guessing at candidate bases has now failed twice (0x340 and 0x258 both look
 * like plain latches, not an AD1848 index/data pair - reading indices 0..15
 * returned the last byte written every time).  So stop guessing bases and map
 * the whole surface: for every port in the declared range, classify what the
 * hardware actually does.
 *
 *   .  float   reads FF, writes do not stick     (nothing there)
 *   -  zero    reads 00, writes do not stick     (card drives low, no register)
 *   L  latch   reads back exactly what was written (full 8-bit storage)
 *   P  partial reads back some of what was written (a few bits implemented)
 *   R  r/only  reads non-zero, unchanged by writes  (status or ID)
 *
 * The SHAPE of that map is the point: a codec, a mixer or a UART shows up as a
 * contiguous cluster of a few live ports, and the cluster boundaries say where
 * the chip blocks start - which is what a wrong base guess cannot tell us.
 *
 * Every write is reverted immediately.  Windows stay 32 bytes, 0x2F8-0x2FF
 * (COM2/IR) is skipped, and 0x3E0-0x3E7 / 0x3F8-0x3FF are refused outright.
 * Run once with 0x342 bit 7 clear and once set, and diff.
 *
 * Build:  C:\WATCOM\BLD IBM16TR6
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
static char kind[NP], kind2[NP];
static unsigned char val[NP], val2[NP];

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

static struct { unsigned s, e; } seg[] = {
    { 0x200, 0x21F }, { 0x220, 0x23F }, { 0x240, 0x25F }, { 0x260, 0x27F },
    { 0x280, 0x29F }, { 0x2A0, 0x2BF }, { 0x2C0, 0x2DF }, { 0x2E0, 0x2F7 },
    { 0x300, 0x31F }, { 0x320, 0x33F }, { 0x340, 0x347 }, { 0, 0 }
};

static void classify(char *k, unsigned char *v)
{
    int i;
    unsigned a;
    for (i = 0; i < NP; i++) { k[i] = ' '; v[i] = 0; }
    for (i = 0; seg[i].e; i++) {
        unsigned s = seg[i].s, e = seg[i].e;
        if (io_range_unsafe(s, e)) continue;
        wr(0x06, rd(0x06) & ~0x40);
        wr(0x08, s & 0xFF); wr(0x09, (s >> 8) & 0xFF);
        wr(0x0A, e & 0xFF); wr(0x0B, (e >> 8) & 0xFF);
        wr(0x07, 0x00);
        wr(0x06, rd(0x06) | 0x40);
        MS(2);
        for (a = s; a <= e; a++) {
            unsigned char o, r1, r2;
            int j = a - LO;
            o  = (unsigned char)inp(a);
            outp(a, 0xAA); r1 = (unsigned char)inp(a);
            outp(a, 0x55); r2 = (unsigned char)inp(a);
            outp(a, o);
            v[j] = o;
            if (r1 == 0xAA && r2 == 0x55)                     k[j] = 'L';
            else if (r1 != r2)                                k[j] = 'P';
            else if (o == 0xFF && r1 == 0xFF)                 k[j] = '.';
            else if (o == 0x00 && r1 == 0x00)                 k[j] = '-';
            else                                              k[j] = 'R';
        }
    }
    wr(0x06, rd(0x06) & ~0x40);
}

static void report(char *tag, char *k, unsigned char *v)
{
    int i, n = 0;
    printf("%s (L=latch P=partial R=read-only, . and - omitted)\n", tag);
    for (i = 0; i < NP; i++) {
        if (k[i] == 'L' || k[i] == 'P' || k[i] == 'R') {
            printf("  %03X %c rest=%02X", LO + i, k[i], v[i]);
            if ((++n % 4) == 0) printf("\n");
        }
    }
    if (n % 4) printf("\n");
    if (!n) printf("  (nothing live)\n");
    printf("  %d live ports\n", n);
}

int main(int argc, char **argv)
{
    unsigned char idrev, ifs;
    unsigned start, stop, woff;
    int i, n;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;

    printf("IBM16TR6 - full read/write map\n");
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
    printf("COR=%02X CCSR=%02X\n\n", (unsigned char)*cor, (unsigned char)*(cor + 2));

    classify(kind, val);
    report("== gate CLOSED ==", kind, val);

    wr(0x06, rd(0x06) & ~0x40);
    wr(0x08, 0x40); wr(0x09, 0x03); wr(0x0A, 0x47); wr(0x0B, 0x03);
    wr(0x07, 0x00); wr(0x06, rd(0x06) | 0x40); MS(2);
    outp(0x342, (unsigned char)(inp(0x342) | 0x80)); MS(50);
    printf("\n342 now %02X\n", (unsigned char)inp(0x342));

    classify(kind2, val2);
    report("\n== gate OPEN ==", kind2, val2);

    printf("\n== differences ==\n");
    n = 0;
    for (i = 0; i < NP; i++)
        if (kind[i] != kind2[i] || val[i] != val2[i]) {
            printf("  %03X %c/%02X -> %c/%02X\n", LO + i, kind[i], val[i], kind2[i], val2[i]);
            n++;
        }
    if (!n) printf("  (none)\n");

    printf("\ndone - card left enabled\n");
    return 0;
}
