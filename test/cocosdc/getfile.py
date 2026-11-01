#!/usr/bin/env python3
"""Print a file from a 35 track Disk BASIC image, with CRs as newlines.

usage: getfile.py IMAGE NAME.EXT
"""

import sys


def main():
    img = open(sys.argv[1], 'rb').read()
    name, _, ext = sys.argv[2].upper().partition('.')
    want = name.ljust(8).encode() + ext.ljust(3).encode()

    def sector(track, sec):
        o = (track * 18 + sec - 1) * 256
        return img[o:o + 256]

    fat = sector(17, 2)
    for s in range(3, 12):
        d = sector(17, s)
        for e in range(0, 256, 32):
            ent = d[e:e + 32]
            if ent[0] in (0, 0xff) or ent[:11] != want:
                continue
            gran = ent[13]
            last = (ent[14] << 8) | ent[15]
            out = bytearray()
            while True:
                track = gran // 2
                if track >= 17:
                    track += 1
                first = 1 + (gran & 1) * 9
                nxt = fat[gran]
                if nxt >= 0xc0:
                    n = nxt & 0x0f
                    for i in range(n):
                        data = sector(track, first + i)
                        out += data if i < n - 1 else data[:last]
                    break
                for i in range(9):
                    out += sector(track, first + i)
                gran = nxt
            sys.stdout.write(out.decode('latin-1').replace('\r', '\n'))
            return
    sys.exit(f'{sys.argv[2]}: not found')


if __name__ == '__main__':
    main()
