/* IBM16TR2.C - IBM NON-DSP AUDIO, take two: why does the card decode but
 * read back all-zero?
 *
 * IBM16TRY left the card enabled (COR=01) with its declared I/O ranges mapped,
 * and every port in 0x220-0x22F and 0x330-0x34F read 0x00 - except 0x342,
 * which read 0x1A and then 0x0A after the block was poked.  0x00 everywhere
 * is ambiguous: it is either the card answering with zeros (function held in
 * reset) or the host returning zeros for a cycle no card claimed.
 *
 * This probe settles that and works the ladder:
 *   CTRL  a window on 0x350-0x36F - outside BOTH declared ranges, so whatever
 *         an unclaimed cycle reads on this host, it reads there.  That is the
 *         reference value every other dump is compared against.
 *   COR   windows mapped FIRST, dumped with the COR still zero, then again
 *         after COR=01 - so the COR's effect is isolated from the window's.
 *   SRST  COR bit7 pulsed (soft reset) before re-writing the config index.
 *   CCSR  audio-enable / power-down bits, read and set.
 *   I16   the same block through a 16-bit window, byte AND word reads - the
 *         Turtle Beach card in this collection needs word cycles.
 *   RW    walking-bit read/write map of 0x340-0x346, each write reverted.
 *
 * Build:  C:\WATCOM\BLD IBM16TR2
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC    0x3E0
#define ATTRSEG 0xD000
#define CTRLS   0x350           /* undeclared, safe: clear of 3E0 and 3F8    */
#define CTRLE   0x35F
#define BLKS    0x340           /* declared range 1                          */
#define BLKE    0x347

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

static void line(char *tag, unsigned s, unsigned e)
{
    unsigned a;
    printf("%-6s %03X:", tag, s);
    for (a = s; a <= e; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\n");
}

static void both(char *tag)
{
    line(tag, BLKS, BLKE);
    line("  ctrl", CTRLS, CTRLS + 7);
}

int main(int argc, char **argv)
{
    unsigned char idrev, ifs, ccsr;
    unsigned start, stop, woff, a;
    int i, b;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;

    printf("IBM16TR2 - decode vs no-answer discrimination\n");
    idrev = rd(0x00); ifs = rd(0x01);
    printf("PCIC IDREV=%02X IntfStat=%02X\n", idrev, ifs);
    if ((idrev & 0xC0) != 0x80) { printf("not an 82365 - abort\n"); return 1; }
    if ((ifs & 0x0C) != 0x0C)   { printf("no card - abort\n"); return 1; }
    if (io_range_unsafe(BLKS, BLKE) || io_range_unsafe(CTRLS, CTRLE)) {
        printf("REFUSED: window spans link or PCIC\n"); return 1;
    }

    /* fresh socket: power-cycle so the COR really starts at zero */
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
    printf("CIS byte0=%02X after %d polls\n",
           (unsigned char)*(unsigned char __far *)MK_FP(ATTRSEG,0), i);

    cor = (unsigned char __far *)MK_FP(ATTRSEG, 0xFFF0);

    /* windows mapped BEFORE the COR is touched */
    wr(0x08, BLKS & 0xFF);  wr(0x09, (BLKS >> 8) & 0xFF);
    wr(0x0A, BLKE & 0xFF);  wr(0x0B, (BLKE >> 8) & 0xFF);
    wr(0x0C, CTRLS & 0xFF); wr(0x0D, (CTRLS >> 8) & 0xFF);
    wr(0x0E, CTRLE & 0xFF); wr(0x0F, (CTRLE >> 8) & 0xFF);
    wr(0x07, 0x00);                       /* both windows 8-bit               */
    wr(0x03, 0x60);
    wr(0x06, 0xC1); MS(20);

    printf("\n== windows mapped, COR still %02X ==\n", (unsigned char)*cor);
    both("cor0");

    *cor = 0x01; MS(50);
    printf("\n== COR <- 01 (reads %02X) ==\n", (unsigned char)*cor);
    both("cor1");

    *cor = 0x80; MS(20); *cor = 0x00; MS(50); *cor = 0x01; MS(100);
    printf("\n== SRESET pulsed, COR <- 01 (reads %02X) ==\n", (unsigned char)*cor);
    both("srst");

    ccsr = (unsigned char)*(cor + 2);
    printf("\n== CCSR reads %02X (bit2 PwrDwn, bit3 Audio, bit5 IOis8) ==\n", ccsr);
    *(cor + 2) = 0x08; MS(50);            /* audio enable, power-down clear    */
    printf("CCSR <- 08, reads %02X\n", (unsigned char)*(cor + 2));
    both("ccsr");

    printf("\n== 16-bit window (reg07 <- 0x22, IOCS16 from card) ==\n");
    wr(0x07, 0x22); MS(20);
    both("io16");
    printf("word  %03X:", BLKS);
    for (a = BLKS; a <= BLKE; a += 2) printf(" %04X", (unsigned)inpw(a));
    printf("\n");
    printf("wctrl %03X:", CTRLS);
    for (a = CTRLS; a <= CTRLS + 7; a += 2) printf(" %04X", (unsigned)inpw(a));
    printf("\n");
    wr(0x07, 0x00); MS(20);

    printf("\n== walking-bit R/W map, %03X-%03X (each write reverted) ==\n",
           BLKS, BLKE - 1);
    for (a = BLKS; a < BLKE; a++) {
        unsigned char orig = (unsigned char)inp(a), stuck1 = 0, stuck0 = 0;
        for (b = 0; b < 8; b++) {
            unsigned char m = (unsigned char)(1 << b), v;
            outp(a, (unsigned char)(orig | m));  v = (unsigned char)inp(a);
            if (v & m) stuck1 |= m;                       /* bit can read 1   */
            outp(a, (unsigned char)(orig & ~m)); v = (unsigned char)inp(a);
            if (!(v & m)) stuck0 |= m;                    /* bit can read 0   */
            outp(a, orig);
        }
        printf("  %03X orig=%02X  writable(both states)=%02X  now=%02X\n",
               a, orig, (unsigned char)(stuck1 & stuck0), (unsigned char)inp(a));
    }

    printf("\ndone - card left enabled\n");
    return 0;
}
