#!/usr/bin/env python3
"""Generates the graphics of the nwsnes test ROM (all original, procedural).

Outputs palette.bin, tiles4.bin, tiles2.bin, sprites.bin, bg1map.bin,
bg2map.bin, bg3map.bin, sine.bin, blob.bin in the given directory.
"""
import math, random, struct, sys, os
out = sys.argv[1] if len(sys.argv) > 1 else '.'
rnd = random.Random(1234)

def rgb(r, g, b):  # 0-31 each -> BGR555
    return r | (g << 5) | (b << 10)

def planar(pix, bpp):
    """8x8 list of colour indices -> SNES bitplane tile"""
    data = bytearray()
    for pair in range(0, bpp, 2):
        for y in range(8):
            b0 = b1 = 0
            for x in range(8):
                c = pix[y][x]
                b0 |= ((c >> pair) & 1) << (7 - x)
                b1 |= ((c >> (pair + 1)) & 1) << (7 - x)
            data += bytes([b0, b1])
    return bytes(data)

# ---------------------------------------------------------------- palette --
pal = [0] * 256
pal[0] = rgb(8, 16, 28)                      # backdrop: sky
# bg palette 0: ground
pal[1:8] = [rgb(4, 3, 2), rgb(12, 7, 3), rgb(18, 11, 5), rgb(24, 16, 8), rgb(6, 20, 6), rgb(10, 26, 9), rgb(28, 28, 24)]
# bg palette 1: bricks / blocks
pal[17:24] = [rgb(5, 2, 2), rgb(20, 6, 4), rgb(26, 10, 6), rgb(30, 18, 10), rgb(31, 26, 8), rgb(31, 31, 20), rgb(12, 12, 14)]
# bg palette 2: far hills and clouds (bg2)
pal[33:40] = [rgb(6, 12, 20), rgb(9, 16, 22), rgb(12, 20, 24), rgb(22, 26, 30), rgb(28, 30, 31), rgb(31, 31, 31), rgb(16, 22, 28)]
# bg3 (2bpp) palettes: hud text
hud = [rgb(0, 0, 0), rgb(31, 31, 31), rgb(31, 24, 4)]
# 2bpp palette n uses colours 4n..4n+3; palette 6 -> 24..27, palette 7 -> 28..31
pal[25:28] = hud
pal[29:32] = [rgb(0, 0, 0), rgb(10, 31, 12), rgb(31, 8, 8)]
# sprite palettes 0-7 at 128+16n
for n in range(8):
    base = 128 + 16 * n
    hue = n / 8.0
    for i in range(1, 16):
        k = i / 15.0
        r = int(31 * k * (0.5 + 0.5 * math.cos(2 * math.pi * hue)))
        g = int(31 * k * (0.5 + 0.5 * math.cos(2 * math.pi * (hue + 0.33))))
        b = int(31 * k * (0.5 + 0.5 * math.cos(2 * math.pi * (hue + 0.66))))
        pal[base + i] = rgb(min(31, r + 6 * (i == 15)), min(31, g + 6 * (i == 15)), min(31, b + 6 * (i == 15)))
open(os.path.join(out, 'palette.bin'), 'wb').write(struct.pack('<256H', *pal))

# ------------------------------------------------------------ 4bpp tiles --
tiles4 = []
def tile(fn):
    tiles4.append([[fn(x, y) & 15 for x in range(8)] for y in range(8)])
tile(lambda x, y: 0)                                                   # 0 empty
tile(lambda x, y: 5 if y < 2 else (6 if y == 2 and (x * 3) % 5 < 2 else 2 + ((x * 7 + y * 3) % 3)))  # 1 grass top
tile(lambda x, y: 2 + ((x * 5 + y * 7 + (x * y) % 3) % 3))            # 2 dirt
tile(lambda x, y: 1 if (y % 4 == 3 or (x + (4 if (y // 4) % 2 else 0)) % 8 == 7) else 2 + (y // 4) % 2)  # 3 brick
tile(lambda x, y: 1 if x in (0, 7) or y in (0, 7) else (5 if (x in (2, 5) and y in (2, 5)) else 4))      # 4 block
tile(lambda x, y: 7 if (x - 3.5) ** 2 + (y - 3.5) ** 2 < 9 else 0)   # 5 dot
tile(lambda x, y: 1 if x in (3, 4) else 0)                             # 6 pole
tile(lambda x, y: 3 if (x + y) % 4 == 0 else 2)                        # 7 slope pattern
# 8-15: hills (bg2) and clouds
tile(lambda x, y: 1 + min(2, (y // 3)))                                # 8 hill body
tile(lambda x, y: 0 if y < 7 - x else 1 + min(2, (y + x - 7) // 3))    # 9 hill left slope
tile(lambda x, y: 0 if y < x else 1 + min(2, (y - x) // 3))            # 10 hill right slope
tile(lambda x, y: 4 if (x - 3.5) ** 2 / 14 + (y - 5) ** 2 / 9 < 1 else 0)   # 11 cloud bit
tile(lambda x, y: 5 if y > 2 else (4 if y > 0 else 0))                 # 12 cloud body
tile(lambda x, y: 6 if (x * x + y * y) % 11 < 4 else 0)                # 13 sparkle
tile(lambda x, y: 2 if y == 7 else 0)                                  # 14 line
tile(lambda x, y: 3)                                                   # 15 solid
# pad with noisy variants so the tile set is a realistic size (128 tiles = 4 KB)
while len(tiles4) < 128:
    k = len(tiles4)
    tile(lambda x, y, k=k: (x * k + y * (k // 3 + 1) + (x ^ y)) % 7 + 1 if (x + y + k) % 3 else 0)
open(os.path.join(out, 'tiles4.bin'), 'wb').write(b''.join(planar(t, 4) for t in tiles4))

# ------------------------------------------------------------ 2bpp tiles --
FONT = {
 ' ': ["     "] * 7,
 '0': [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
 '1': ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
 '2': [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
 '3': [" ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### "],
 '4': ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
 '5': ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
 '6': [" ### ", "#    ", "#### ", "#   #", "#   #", "#   #", " ### "],
 '7': ["#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "],
 '8': [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
 '9': [" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "],
 'A': [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
 'B': ["#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "],
 'C': [" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "],
 'D': ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
 'E': ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
 'F': ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
 'M': ["#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"],
 'N': ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
 'O': [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
 'P': ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
 'R': ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
 'S': [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
 'T': ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
 'U': ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
 'W': ["#   #", "#   #", "#   #", "#   #", "# # #", "## ##", "#   #"],
 'X': ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
}
CHARS = " 0123456789ABCDEFMNOPRSTUWX"
tiles2 = []
for ch in CHARS:
    g = FONT[ch]
    pix = [[0] * 8 for _ in range(8)]
    for y in range(7):
        for x in range(5):
            if g[y][x] == '#':
                pix[y][x + 1] = 2
    tiles2.append(pix)
while len(tiles2) < 64:
    tiles2.append([[0] * 8 for _ in range(8)])
open(os.path.join(out, 'tiles2.bin'), 'wb').write(b''.join(planar(t, 2) for t in tiles2))
def text(s): return [CHARS.index(c) for c in s]

# -------------------------------------------------------------- sprites --
# 16x16 ball with shading, stored the way the PPU wants it: a 16x16 sprite
# with tile number n uses tiles n, n+1, n+16, n+17 of a 16-tile wide sheet.
sheet = [[[0] * 8 for _ in range(8)] for _ in range(64)]   # 4 rows of 16 tiles
def put(shape, tcol, trow):
    for y in range(16):
        for x in range(16):
            sheet[(trow + y // 8) * 16 + tcol + x // 8][y % 8][x % 8] = shape(x, y)
def ball(x, y):
    d = math.hypot(x - 7.5, y - 7.5)
    if d > 7.6: return 0
    light = max(0.0, 1 - math.hypot(x - 5, y - 4.5) / 9)
    return max(1, min(15, int(3 + 12 * light)))
def star(x, y):
    dx, dy = abs(x - 7.5), abs(y - 7.5)
    return 15 if dx + dy < 4 else (9 if (dx < 1.2 or dy < 1.2) and dx + dy < 8 else 0)
def box(x, y):
    return 0 if x in (0, 15) or y in (0, 15) else (14 if x in (1, 14) or y in (1, 14) else 4 + ((x + y) // 4) % 4)
put(ball, 0, 0); put(star, 2, 0); put(box, 4, 0)
open(os.path.join(out, 'sprites.bin'), 'wb').write(b''.join(planar(t, 4) for t in sheet))

# ----------------------------------------------------------------- maps --
def entry(tile, pal=0, prio=0, hflip=0, vflip=0):
    return tile | (pal << 10) | (prio << 13) | (hflip << 14) | (vflip << 15)

# bg1: 64x32, the playfield (two screens wide)
bg1 = [[0] * 64 for _ in range(32)]
height = [22] * 64
h = 22
for x in range(64):
    if x % 6 == 5: h = max(18, min(24, h + rnd.choice((-1, 0, 0, 1))))
    height[x] = h
for x in range(64):
    for y in range(32):
        if y == height[x]: bg1[y][x] = entry(1)
        elif y > height[x]: bg1[y][x] = entry(2 if (x + y) % 5 else 7)
for px in range(4, 60, 9):
    py = height[px] - rnd.randint(4, 7)
    for i in range(rnd.randint(3, 5)):
        bg1[py][(px + i) % 64] = entry(3, pal=1)
    bg1[py - 3][(px + 1) % 64] = entry(4, pal=1, prio=1)
for x in range(2, 64, 11):
    for y in range(height[x] - 5, height[x]):
        bg1[y][x] = entry(6)
    bg1[height[x] - 6][x] = entry(5, pal=1)
flat = [e for row in bg1 for e in row]
# the PPU stores a 64x32 map as two 32x32 screens
left = [bg1[y][x] for y in range(32) for x in range(32)]
right = [bg1[y][x] for y in range(32) for x in range(32, 64)]
open(os.path.join(out, 'bg1map.bin'), 'wb').write(struct.pack('<2048H', *(left + right)))

# bg2: 32x32, hills and clouds
bg2 = [[0] * 32 for _ in range(32)]
for cx, top, w in ((4, 16, 5), (14, 13, 7), (25, 17, 4)):
    for y in range(top, 32):
        half = w + (y - top)
        for x in range(cx - half, cx + half + 1):
            t = 8
            if x == cx - half: t = 9
            if x == cx + half: t = 10
            bg2[y][x % 32] = entry(t, pal=2)
for cx, cy in ((3, 4), (11, 7), (19, 3), (27, 6), (8, 10), (23, 10)):
    bg2[cy][cx % 32] = entry(11, pal=2)
    bg2[cy][(cx + 1) % 32] = entry(12, pal=2)
    bg2[cy][(cx + 2) % 32] = entry(11, pal=2, hflip=1)
for _ in range(14):
    bg2[rnd.randint(0, 11)][rnd.randint(0, 31)] = entry(13, pal=2)
open(os.path.join(out, 'bg2map.bin'), 'wb').write(struct.pack('<1024H', *[e for row in bg2 for e in row]))

# bg3: 32x32, the HUD (2bpp, drawn above everything)
bg3 = [[0] * 32 for _ in range(32)]
def write(x, y, s, pal):
    for i, t in enumerate(text(s)):
        bg3[y][x + i] = entry(t, pal=pal, prio=1)
write(2, 2, "NWSNES TEST", 6)
write(2, 3, "FRAME 0000", 7)     # digits at x=8..11 are rewritten every frame
write(18, 2, "PAD 0000", 6)      # hex digits at x=22..25 show the buttons
open(os.path.join(out, 'bg3map.bin'), 'wb').write(struct.pack('<1024H', *[e for row in bg3 for e in row]))

# ----------------------------------------------------------------- misc --
open(os.path.join(out, 'sine.bin'), 'wb').write(bytes(int(127.5 + 127.5 * math.sin(2 * math.pi * i / 256)) & 255 for i in range(256)))
# a blob the ROM copies into work RAM at start-up, like games that keep
# decompressed graphics there (8 KB that then never changes)
open(os.path.join(out, 'blob.bin'), 'wb').write(bytes(rnd.randrange(256) if (i // 32) % 3 else (i * 7) & 255 for i in range(8192)))
print('assets written to', out)
