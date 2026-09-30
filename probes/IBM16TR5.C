/* IBM16TR5.C - IBM NON-DSP AUDIO: identify the codec at 0x258.
 *
 * TR4's index-register hunt found an Index Address Register that echoes bits
 * 5:0 at 0x258, with 0x259 storing through it and 0x25A sitting at 0xA0 - the
 * classic AD1848 / CS4231 four-register block (IAR / IDR / Status / PIO).
 * Nothing at 0x340-0x347 or 0x220-0x22F behaves like a codec, so the block the
 * CIS hull pointed at is not where the audio lives.
 *
 * This probe runs the full identification: INIT, index echo, register-file
 * scratch, the mode-1 dump, the I12 signature (0x8A = CS4248/AD1848 lineage),
 * and the MODE2 attempt whose I25 version byte separates CS4231 (0x80) from
 * CS4231A (0xA0).  Everything is done twice - with 0x342 bit 7 clear and set -
 * because that bit gates at least one other register on this card.
 *
 * Build:  C:\WATCOM\BLD IBM16TR5
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC    0x3E0
#define ATTRSEG 0xD000
#define CB      0x258           /* codec base found by TR4                    */

static unsigned pidx = PCIC, soff = 0;
static unsigned char __far *cor;

static unsigned char rd(unsigned r){ outp(pidx, soff + r); return (unsigned char)inp(pidx + 1); }
static void          wr(unsigned r, unsigned v){ outp(pidx, soff + r); outp(pidx + 1, v); }
static void dly(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) dly((unsigned long)(x) * 1000UL)

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
static void cwrite_mce(unsigned cb, unsigned char idx, unsigned char val)
{
    cwait(cb); outp(cb, (unsigned char)(0x40 | idx)); outp(cb + 1, val);
    cwait(cb); outp(cb, idx);
}

static void identify(unsigned cb, char *when)
{
    unsigned char i12, i25, t1, t2, r1, r2;
    int i;

    printf("\n===== codec at %03X, %s =====\n", cb, when);

    outp(cb, 0x0A); r1 = (unsigned char)inp(cb);
    outp(cb, 0x15); r2 = (unsigned char)inp(cb);
    printf("IAR echo: 0A->%02X 15->%02X   status(%03X)=%02X  pio(%03X)=%02X\n",
           r1, r2, cb + 2, (unsigned char)inp(cb + 2),
           cb + 3, (unsigned char)inp(cb + 3));
    printf("INIT: %s\n", cwait(cb) ? "clear" : "STUCK");

    cwrite(cb, 0x00, 0xAA); t1 = cread(cb, 0x00);
    cwrite(cb, 0x00, 0x55); t2 = cread(cb, 0x00);
    printf("I0 scratch: AA->%02X 55->%02X  %s\n", t1, t2,
           (t1 != t2) ? "register file LIVE" : "dead");

    printf("mode1 I0-15 :");
    for (i = 0; i < 16; i++) printf(" %02X", cread(cb, (unsigned char)i));
    printf("\n");

    i12 = cread(cb, 0x0C);
    printf("I12=%02X %s\n", i12,
           ((i12 & 0x8F) == 0x8A) ? "<== 0x8A: CS4248 / AD1848 signature" : "");

    cwrite_mce(cb, 0x0C, (unsigned char)(i12 | 0x40));
    i12 = cread(cb, 0x0C);
    if (i12 & 0x40) {
        printf("MODE2 accepted (I12=%02X)\nmode2 I16-31:", i12);
        for (i = 16; i < 32; i++) printf(" %02X", cread(cb, (unsigned char)i));
        printf("\n");
        i25 = cread(cb, 0x19);
        printf("I25 version=%02X : ", i25);
        switch (i25 & 0xE0) {
            case 0x80: printf("CS4231\n");  break;
            case 0xA0: printf("CS4231A\n"); break;
            default:   printf("unknown mode-2 part\n"); break;
        }
        cwrite_mce(cb, 0x0C, (unsigned char)(i12 & ~0x40));   /* back to mode 1 */
    } else {
        printf("MODE2 REFUSED (I12=%02X) -> mode-1-only: CS4248 / AD1848 class\n", i12);
    }
}

int main(int argc, char **argv)
{
    unsigned char idrev, ifs;
    unsigned start, stop, woff, a;
    int i;

    if (argc > 1 && argv[1][0] == '1') soff = 0x40;

    printf("IBM16TR5 - codec identification at %03X\n", CB);
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

    /* W0 = codec block, W1 = the 0x340 block that holds the gate bit.
     * Both windows programmed with the enable bits down first. */
    wr(0x06, rd(0x06) & ~0xC0);
    wr(0x08, 0x40); wr(0x09, 0x02); wr(0x0A, 0x5F); wr(0x0B, 0x02);   /* 240-25F */
    wr(0x0C, 0x40); wr(0x0D, 0x03); wr(0x0E, 0x47); wr(0x0F, 0x03);   /* 340-347 */
    wr(0x07, 0x00);
    wr(0x06, rd(0x06) | 0xC0); MS(5);

    printf("\nio 250-25F:");
    for (a = 0x250; a <= 0x25F; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\nio 340-347:");
    for (a = 0x340; a <= 0x347; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\n");

    identify(CB, "gate CLOSED (342 bit7 = 0)");

    printf("\n-- opening gate: 342 %02X -> ", (unsigned char)inp(0x342));
    outp(0x342, (unsigned char)(inp(0x342) | 0x80)); MS(50);
    printf("%02X --\n", (unsigned char)inp(0x342));

    identify(CB, "gate OPEN (342 bit7 = 1)");

    printf("\nio 250-25F:");
    for (a = 0x250; a <= 0x25F; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\nio 340-347:");
    for (a = 0x340; a <= 0x347; a++) printf(" %02X", (unsigned char)inp(a));
    printf("\n\ndone - card left enabled\n");
    return 0;
}
