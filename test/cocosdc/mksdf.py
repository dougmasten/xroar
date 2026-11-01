#!/usr/bin/env python3
"""Make a double density SDF disk image for the CoCo SDC tests.

Each track has 18 256-byte sectors.  Sector data is LSN-derived, so a read
can be checked: every byte of LSN n is (n + offset) & 0xff.

usage: mksdf.py [options] FILE
  -t N          cylinders (default 35)
  -s N          sides (default 1)
  -w            set the write protect byte
  --id-crc LSN  give this sector's ID field a bad CRC (may repeat)
  --data-crc LSN  give this sector's data field a bad CRC (may repeat)
  --missing LSN   leave this sector out (may repeat)
"""

import argparse

TRACK_SIZE = 6656
TABLE_SIZE = 256
DATA_SIZE = TRACK_SIZE - TABLE_SIZE
SECTORS = 18


def crc16(data, crc=0xffff):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xffff
    return crc


def track(cyl, head, sides, args):
    t = bytearray(b'\x4e' * 32)
    table = []
    for s in range(SECTORS):
        lsn = (cyl * sides + head) * SECTORS + s
        if lsn in args.missing:
            continue
        t += b'\x00' * 12
        field = bytes([0xa1, 0xa1, 0xa1, 0xfe, cyl, head, s + 1, 1])
        crc = crc16(field)
        if lsn in args.id_crc:
            crc ^= 0x5555
        id_off = len(t) + 4
        t += field + bytes([crc >> 8, crc & 0xff]) + b'\x4e' * 22 + b'\x00' * 12
        field = bytes([0xa1, 0xa1, 0xa1, 0xfb]) + bytes([(lsn + i) & 0xff for i in range(256)])
        crc = crc16(field)
        if lsn in args.data_crc:
            crc ^= 0x5555
        data_off = len(t) + 4
        t += field + bytes([crc >> 8, crc & 0xff]) + b'\x4e' * 24
        table.append((id_off, data_off, lsn in args.id_crc, lsn in args.data_crc, (cyl, head, s + 1, 1)))
    t += b'\x4e' * (DATA_SIZE - len(t))
    rec = bytearray(TABLE_SIZE)
    rec[0] = len(table)
    for i, (id_off, data_off, id_bad, data_bad, chrn) in enumerate(table):
        a = (TABLE_SIZE + id_off) | (0x8000 if id_bad else 0)
        b = (TABLE_SIZE + data_off) | (0x8000 if data_bad else 0)
        rec[8 + i * 8:16 + i * 8] = bytes([a & 0xff, a >> 8, b & 0xff, b >> 8]) + bytes(chrn)
    return bytes(rec) + bytes(t)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('-t', type=int, default=35)
    p.add_argument('-s', type=int, default=1)
    p.add_argument('-w', action='store_true')
    p.add_argument('--id-crc', type=int, action='append', default=[])
    p.add_argument('--data-crc', type=int, action='append', default=[])
    p.add_argument('--missing', type=int, action='append', default=[])
    p.add_argument('file')
    args = p.parse_args()
    header = bytearray(512)
    header[0:4] = b'SDF1'
    header[4] = args.t
    header[5] = args.s
    header[6] = 0xff if args.w else 0
    with open(args.file, 'wb') as f:
        f.write(header)
        for c in range(args.t):
            for h in range(args.s):
                f.write(track(c, h, args.s, args))


if __name__ == '__main__':
    main()
