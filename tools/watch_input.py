#!/usr/bin/env python3
"""Live monitor: prints every distinct (buttons, sticks) state as it arrives.

Usage: watch_input.py [secs]
Runs until the timeout, so the user can press things at their own pace.
"""
import sys, time, collections, hid

VID, PID = 0x057E, 0x2009
B3 = [(0x01, 'Y'), (0x02, 'X'), (0x04, 'B'), (0x08, 'A'), (0x40, 'R'), (0x80, 'ZR')]
B4 = [(0x01, '-'), (0x02, '+'), (0x04, 'RS'), (0x08, 'LS'), (0x10, 'Home'), (0x20, 'Cap')]
B5 = [(0x01, 'Down'), (0x02, 'Up'), (0x04, 'Right'), (0x08, 'Left'), (0x40, 'L'), (0x80, 'ZL')]


def state(b):
    names = [n for m, n in B3 if b[3] & m] + [n for m, n in B4 if b[4] & m] + \
            [n for m, n in B5 if b[5] & m]
    lx = b[6] | ((b[7] & 0xF) << 8)
    ly = (b[7] >> 4) | (b[8] << 4)
    rx = b[9] | ((b[10] & 0xF) << 8)
    ry = (b[10] >> 4) | (b[11] << 4)
    btn = ','.join(names) or '-'
    return btn, '%d,%d %d,%d' % (lx, ly, rx, ry)


def main(secs):
    path = [x for x in hid.enumerate(VID, PID)][0]['path']
    dev = hid.device()
    dev.open_path(path)
    dev.set_nonblocking(1)
    t0 = time.time()
    last = None
    seen = collections.Counter()
    while time.time() - t0 < secs:
        b = dev.read(64, 100)
        if not b or b[0] != 0x30:
            continue
        st = state(b)
        seen[st] += 1
        if st != last:
            print('%5.1fs  %-28s sticks %s' % (time.time() - t0, st[0], st[1]))
            sys.stdout.flush()
            last = st
    dev.close()
    print('--- %d distinct states, %d frames' % (len(seen), sum(seen.values())))


if __name__ == '__main__':
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 45.0)
