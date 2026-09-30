/* CARDPWR.C - power a PCMCIA socket down (or up) on an Intel 82365-class PCIC.
 *
 * A PC Card's Configuration Option Register is volatile but it lives on the
 * CARD, so a warm reboot does not clear it - the host's PCIC resets, the card
 * keeps whatever config the last program wrote.  A vendor driver that expects
 * to configure a fresh card then reports it missing.  Only removing Vcc
 * resets the card, so this drops power, waits, and (by default) leaves the
 * socket off for the next driver to bring up cleanly.
 *
 *   CARDPWR          power socket 0 off  (default)
 *   CARDPWR /ON      power it back up, memory mode, reset released
 *   CARDPWR /CYCLE   off, pause, on
 *   add /S1 for socket 1
 *
 * Build:  C:\WATCOM\BLD CARDPWR
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC 0x3E0
static unsigned soff = 0;

static unsigned char rd(unsigned r)
{
    unsigned char v;
    _disable(); outp(PCIC, soff + r); v = (unsigned char)inp(PCIC + 1); _enable();
    return v;
}
static void wr(unsigned r, unsigned v)
{
    _disable(); outp(PCIC, soff + r); outp(PCIC + 1, v); _enable();
}
static void dly(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) dly((unsigned long)(x) * 1000UL)

static void off(void)
{
    wr(0x06, 0x00);          /* all windows off first  */
    wr(0x03, 0x00);          /* reset asserted, mem mode, no IRQ */
    wr(0x02, 0x00);          /* Vcc and Vpp off        */
    MS(500);                 /* let the card fully discharge     */
}
static void on(void)
{
    wr(0x02, 0x95); MS(50);  /* 5V, output enable      */
    wr(0x03, 0x40); MS(20);  /* release reset, memory mode       */
}

int main(int argc, char **argv)
{
    int i, want_on = 0, cycle = 0;
    unsigned char id;

    for (i = 1; i < argc; i++) {
        char c1 = argv[i][1], c2 = argv[i][2];
        if (c1 == 'O' || c1 == 'o') { if (c2 == 'N' || c2 == 'n') want_on = 1; }
        if (c1 == 'C' || c1 == 'c') cycle = 1;
        if (c1 == 'S' || c1 == 's') { if (c2 == '1') soff = 0x40; }
    }

    id = rd(0x00);
    if ((id & 0xC0) != 0x80) { printf("no 82365-class PCIC at %03X\n", PCIC); return 1; }
    printf("PCIC IDREV=%02X socket %d: status=%02X power=%02X intctl=%02X\n",
           id, soff ? 1 : 0, rd(0x01), rd(0x02), rd(0x03));

    if (cycle)        { off(); on();  printf("power cycled, socket left ON\n"); }
    else if (want_on) { on();         printf("socket powered ON\n"); }
    else              { off();        printf("socket powered OFF (card config cleared)\n"); }

    printf("now: status=%02X power=%02X intctl=%02X winen=%02X\n",
           rd(0x01), rd(0x02), rd(0x03), rd(0x06));
    return 0;
}
