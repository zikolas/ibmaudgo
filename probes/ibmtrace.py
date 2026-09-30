#!/usr/bin/env python3
"""Decode an IBMTRACE /W log.

    ibmtrace.py TRACE.BIN            summary + full event listing
    ibmtrace.py TRACE.BIN --summary  counts only
    ibmtrace.py TRACE.BIN --first N  first N entries only
    ibmtrace.py TRACE.BIN --data 340 OUT.BIN
                                     write the string-I/O data elements sent
                                     to (or read from) port 340h, in order

Timing: the BIOS tick plus one 8254 read-back of channel 0 (count and
status), assuming the BIOS reload of 65536 at 1.193182 MHz.  In mode 3 the
count runs down twice per tick, by 2 per clock; the status OUT bit says
which half.  An entry taken with IRQ 0 pending (tick not yet bumped) would
look like time going backwards; that is corrected by one tick.
"""
import struct
import sys

HDR = 0x323 - 0x103
NPORT, NKIND = 24, 5
ENTSZ = 12
KINDS = ("in b", "in w", "out b", "out w", "other")
PIT_HZ = 1193182.0


def off(mem):
    return mem - 0x103


def typ(cl):
    size = "d" if cl & 0x10 else ("w" if cl & 0x08 else "b")
    if cl & 0x80:
        return ("DATA>" if cl & 4 else "DATA<") + size
    s = ("OUT" if cl & 4 else "IN ") + size
    if cl & 0x20:
        s += " STR"
    if cl & 0x40:
        s += " REP"
    return s


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if data[:4] != b"IBTR":
        sys.exit("not an IBMTRACE log")
    u8 = lambda m: data[off(m)]
    u16 = lambda m: struct.unpack_from("<H", data, off(m))[0]
    u32 = lambda m: struct.unpack_from("<I", data, off(m))[0]
    w0, w1 = u16(0x10A), u16(0x10C)
    nfilt = u8(0x109)
    filt = [u16(0x10E + 2 * i) for i in range(nfilt)]
    nlog, nev, ndrop, nrep, nchain, nfail = (u32(m) for m in (0x124, 0x128, 0x12C, 0x130, 0x134, 0x31C))
    ver = u8(0x107)
    jlm = ver >= 3 and u8(0x322) == 1
    strmax = u16(0x320) if ver >= 3 else 0
    print(f"version {ver}  backend {'IBMTRJ (ring 0)' if jlm else 'QPI'}  blocks {w0:03X}+10h {w1:03X}+8  "
          f"filtered {' '.join(f'{p:03X}' for p in filt) or '-'}  strmax {strmax or 'all'}")
    print(f"events {nev}  logged {nlog}/{u32(0x11C)}  dropped {ndrop}  "
          f"{'rep-ops' if jlm else 'rep-lost'} {nrep}  chained {nchain}  qpi-fail {nfail}")
    print("port " + "".join(f"{k:>10}" for k in KINDS))
    for i in range(NPORT):
        c = [u32(0x138 + (i * NKIND + k) * 4) for k in range(NKIND)]
        if any(c):
            port = w0 + i if i < 16 else w1 + i - 16
            print(f"{port:03X}  " + "".join(f"{v:>10}" for v in c))
    if "--summary" in sys.argv:
        return
    base = HDR if ver >= 3 else 0x320 - 0x103
    if "--data" in sys.argv:
        i = sys.argv.index("--data")
        port, out = int(sys.argv[i + 1], 16), sys.argv[i + 2]
        buf = bytearray()
        for n in range(nlog):
            pt, val, cl, rep, *_ = struct.unpack_from("<HHBBHHBB", data, base + n * ENTSZ)
            if cl & 0x80 and pt == port:
                el = val.to_bytes(2, "little") if cl & 0x18 else bytes([val & 0xFF])
                buf += el * (rep + 1)
        open(out, "wb").write(buf)
        print(f"wrote {len(buf)} bytes of port {port:03X} string data to {out}")
        return
    lim = nlog
    if "--first" in sys.argv:
        lim = min(nlog, int(sys.argv[sys.argv.index("--first") + 1]))
    t0 = prev = None
    print()
    print(f"{'#':>6} {'t ms':>10} {'dt us':>8}  type        port  value  rep")
    wrap = 0
    for n in range(lim):
        port, val, cl, rep, tick, pit, st, _ = struct.unpack_from("<HHBBHHBB", data, base + n * ENTSZ)
        cnt = pit or 65536
        mode = (st >> 1) & 7
        if mode in (3, 7):
            clk = (65536 - cnt) // 2 + (0 if st & 0x80 else 32768)
        else:
            clk = 65536 - cnt
        t = tick * 65536 + clk + wrap
        if prev is not None and t < prev:
            if prev - t <= 65536:
                t += 65536            # IRQ 0 was pending: tick not yet bumped
            else:
                wrap += 1 << 32       # tick low word wrapped
                t += 1 << 32
        if t0 is None:
            t0 = prev = t
        dt = (t - prev) / PIT_HZ * 1e6
        ms = (t - t0) / PIT_HZ * 1e3
        prev = t
        r = f"x{rep + 1}" if rep else ""
        print(f"{n:>6} {ms:>10.3f} {dt:>8.0f}  {typ(cl):<10}  {port:03X}   {val:04X}  {r}")


if __name__ == "__main__":
    main()
