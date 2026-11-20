#!/usr/bin/env python3
"""Writes the SNES header checksum of a LoROM image in place."""
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[0x7fdc:0x7fe0] = b'\xff\xff\x00\x00'
c = sum(d) & 0xffff
d[0x7fdc] = (c ^ 0xffff) & 0xff; d[0x7fdd] = (c ^ 0xffff) >> 8
d[0x7fde] = c & 0xff; d[0x7fdf] = c >> 8
open(p, 'wb').write(d)
print('checksum %04x' % c)
