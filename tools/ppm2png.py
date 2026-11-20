#!/usr/bin/env python3
"""Converts the PPM frames written by nwsnes-host to PNG (needs Pillow).
Usage: ppm2png.py <file-or-dir>... [--scale N] [--sheet out.png]"""
import sys, os, glob
from PIL import Image
args = [a for a in sys.argv[1:] if not a.startswith('--')]
scale = 1; sheet = None
for i, a in enumerate(sys.argv):
    if a == '--scale': scale = int(sys.argv[i + 1]); args.remove(sys.argv[i + 1])
    if a == '--sheet': sheet = sys.argv[i + 1]; args.remove(sys.argv[i + 1])
files = []
for a in args:
    files += sorted(glob.glob(os.path.join(a, '*.ppm'))) if os.path.isdir(a) else [a]
imgs = []
for f in files:
    im = Image.open(f).convert('RGB')
    if scale != 1: im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    imgs.append((f, im))
    if not sheet: im.save(os.path.splitext(f)[0] + '.png')
if sheet and imgs:
    cols = min(4, len(imgs)); rows = (len(imgs) + cols - 1) // cols
    w, h = imgs[0][1].size
    out = Image.new('RGB', (cols * w + (cols + 1) * 4, rows * h + (rows + 1) * 4), (40, 40, 40))
    for i, (_, im) in enumerate(imgs):
        out.paste(im, (4 + (i % cols) * (w + 4), 4 + (i // cols) * (h + 4)))
    out.save(sheet)
print(len(imgs), 'image(s)')
