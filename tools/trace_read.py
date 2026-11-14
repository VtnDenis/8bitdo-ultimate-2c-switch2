"""Read the console trace the patched firmware writes at flash 0x53000.

Pad must be in the bootloader (2dc8:3208): unplug, hold LB+RB, replug into the PC.
Record (32 B): pkt[0..19], len, site, result, ready, drops(2), reply[13..18].
"""
import sys, hid
import flash

BASE, N, REC = 0x53000, 128, 32


def main():
    dev = hid.device()
    dev.open(flash.VID, flash.PID)
    dev.set_nonblocking(0)
    raw = flash.read(dev, BASE, N * REC)
    n = 0
    for i in range(N):
        r = raw[i * REC:(i + 1) * REC]
        if r == b"\xff" * REC:
            break
        drops = r[24] | r[25] << 8
        print("%3d len=%3d site=%#04x res=%#04x rdy=%d drop=%3d "
              "rep=%s pkt=%s"
              % (i, r[20], r[21], r[22], r[23], drops,
                 r[26:32].hex(" "), r[:20].hex(" ")))
        n += 1
    print("--- %d records" % n)


if __name__ == "__main__":
    main()
