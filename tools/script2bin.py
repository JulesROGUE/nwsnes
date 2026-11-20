#!/usr/bin/env python3
"""Converts a text input script ("<frame> <hex buttons>" per line, held until
the next line) into one little-endian u16 per frame, for "make bench INPUTS=".
Usage: script2bin.py <script.txt> <frames> <out.bin>"""
import struct, sys
events = [tuple(l.split()) for l in open(sys.argv[1]) if l.strip()]
n = int(sys.argv[2]); out = []; cur = 0; i = 0
for f in range(n):
    while i < len(events) and int(events[i][0]) <= f:
        cur = int(events[i][1], 16); i += 1
    out.append(cur)
open(sys.argv[3], 'wb').write(struct.pack('<%dH' % n, *out))
