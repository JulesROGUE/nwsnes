#!/usr/bin/env python3
"""One-off helper: derives two files from LakeSnes' ppu.c (MIT).

Usage: import_ppu.py <LakeSnes/snes/ppu.c> <core dir>

  core/ppu_regs.c  register reads/writes and per-frame bookkeeping, used on
                   the calculator. Video RAM goes through the pager.
  core/ppu_ref.c   LakeSnes' accurate per-pixel renderer, kept as a reference
                   for tests on a PC (never built for the calculator).
"""
import re, sys
src = open(sys.argv[1]).read()
out = sys.argv[2]

def func(name):
    """returns the full text of a function definition"""
    m = re.search(r'^[\w\* ]+\b%s\(.*?\) \{$' % re.escape(name), src, re.M)
    assert m, name
    i = src.index('\n}\n', m.start()) + 3
    return src[m.start():i]

def vram_reads(text, s):
    """ppu->vram[expr] -> snes_vramRead(s, expr), with bracket matching"""
    res = ''
    i = 0
    key = 'ppu->vram['
    while True:
        j = text.find(key, i)
        if j < 0:
            return res + text[i:]
        k = j + len(key); depth = 1
        while depth:
            c = text[k]
            depth += (c == '[') - (c == ']')
            k += 1
        expr = text[j + len(key):k - 1]
        res += text[i:j] + 'snes_vramRead(%s, %s)' % (s, expr)
        i = k

LICENSE = '''/*
 * nwsnes - %s
 *
 * Derived from LakeSnes (https://github.com/angelo-wf/LakeSnes)
 * Copyright (c) 2021-2023 angelo_wf and contributors - MIT license.
 * %s
 */
'''

# ---------------------------------------------------------------- regs --
regs = LICENSE % ('PPU registers', 'Adapted for nwsnes: video RAM lives in the pager.')
regs += '#include <string.h>\n\n#include "snes.h"\n\n'

reset = func('ppu_reset')
reset = reset.replace('void ppu_reset(Ppu* ppu) {', 'void ppu_reset(Snes* s) {\n  Ppu* ppu = &s->ppu;\n  /* video RAM is cleared by pager_reset() on a hard reset */')
reset = reset.replace('  memset(ppu->vram, 0, sizeof(ppu->vram));\n', '')
reset = reset.replace('  memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));\n  memset(ppu->objPriorityBuffer, 0, sizeof(ppu->objPriorityBuffer));\n',
                      '  memset(ppu->objRow, 0, sizeof(ppu->objRow));\n  memset(ppu->objSlivers, 0, sizeof(ppu->objSlivers));\n'
                      '  memset(ppu->objPrioMask, 0, sizeof(ppu->objPrioMask));\n  ppu->renderedLine = 0;\n  ppu->palBlendKey[0] = 0xffffffffu;\n'
                      '  ppu->regKnown = 0;\n  ppu->wlogCount = 0;\n'
                      '  ppu->lutBrightness = 0xff;\n')
reset = reset.replace('  memset(ppu->pixelBuffer, 0, sizeof(ppu->pixelBuffer));\n', '')
assert 'vram' not in reset.replace('vramPointer','').replace('vramIncrement','').replace('vramRemap','').replace('vramRead','').replace('video RAM','')
regs += reset + '\n'

for name in ('ppu_checkOverscan', 'ppu_handleVblank', 'ppu_handleFrameStart'):
    f = func(name)
    f = re.sub(r'%s\(Ppu\* ppu\) \{' % name, '%s(Snes* s) {\n  Ppu* ppu = &s->ppu;' % name, f)
    regs += f + '\n'

regs += func('ppu_getVramRemap') + '\n'

rd = func('ppu_read')
rd = rd.replace('uint8_t ppu_read(Ppu* ppu, uint8_t adr) {', 'uint8_t ppu_read(Snes* s, uint8_t adr) {\n  Ppu* ppu = &s->ppu;')
rd = rd.replace('ppu->vram[ppu_getVramRemap(ppu) & 0x7fff]', 'snes_vramRead(s, ppu_getVramRemap(ppu))')
rd = rd.replace('ppu->snes->', 's->')
rd = rd.replace('ppu->hCount = s->hPos / 4;', 'ppu->hCount = (uint16_t)(snes_hPos(s) / 4);')
assert 'snes_hPos' in rd
# nwsnes: the picture is drawn late; reading the sprite flags or stepping
# through OAM needs it to be up to date first
rd = rd.replace('  Ppu* ppu = &s->ppu;\n', '  Ppu* ppu = &s->ppu;\n  if(ppu->wlogCount || adr == 0x38 || adr == 0x3e) ppu_flush(s);\n', 1)
assert 'ppu->vram[' not in rd
regs += rd + '\n'

wr = func('ppu_write')
wr = wr.replace('void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {', 'void ppu_write(Snes* s, uint8_t adr, uint8_t val) {\n  Ppu* ppu = &s->ppu;')
wr = wr.replace('ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];', 'ppu->vramReadBuffer = snes_vramRead(s, ppu_getVramRemap(ppu));')
wr = wr.replace('''      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;''',
'''      uint32_t vramAdr = ppu_getVramRemap(ppu) & 0x7fff;
      pager_write(&s->pager, NS_VP_VRAM + (vramAdr >> 7), (vramAdr & 0x7f) << 1, val);''')
wr = wr.replace('''      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);''',
'''      uint32_t vramAdr = ppu_getVramRemap(ppu) & 0x7fff;
      pager_write(&s->pager, NS_VP_VRAM + (vramAdr >> 7), ((vramAdr & 0x7f) << 1) | 1, val);''')
wr = wr.replace('ppu->snes->', 's->')
# nwsnes: the picture is drawn late. LakeSnes' function becomes ppu_apply (the
# register write itself); ppu_write decides whether it happens now or is
# written down to be replayed when the lines are drawn.
wr = wr.replace('void ppu_write(Snes* s, uint8_t adr, uint8_t val) {\n  Ppu* ppu = &s->ppu;\n',
                'void ppu_apply(Snes* s, uint8_t adr, uint8_t val) {\n  Ppu* ppu = &s->ppu;\n', 1)
assert 'void ppu_apply(' in wr
wr = '''/* nwsnes: what a write to each register means for the picture.
 * 0: anything;
 * 1: the register just holds the value: writing the same one again changes nothing;
 * 2: the picture does not depend on it (video ram and palette addresses,
 *    video port control, read-only registers). */
static const uint8_t kRegKind[0x40] = {
  1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, /* 00-0f */
  0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 1, 0, 0, 0, 0, 0, /* 10-1f */
  0, 2, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 20-2f */
  1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2  /* 30-3f */
};

/* nwsnes: the picture is drawn late, in one go (see ppu.c). While the
 * visible lines go by, a write that can change the picture is not done
 * right away: it is written down with the line it takes effect after, and
 * ppu_flush replays the list while it draws. The game cannot tell: anything
 * that reads the PPU flushes first (ppu_read). This is what keeps a game that
 * changes a register on every line (HDMA) from interrupting itself 224 times
 * per frame to draw one line. */
void ppu_write(Snes* s, uint8_t adr, uint8_t val) {
  Ppu* ppu = &s->ppu;
  adr &= 0x3f;
  const uint8_t kind = kRegKind[adr];
  const bool same = kind == 1 && ((ppu->regKnown >> adr) & 1) && ppu->regLast[adr] == val;
  ppu->regLast[adr] = val;
  ppu->regKnown |= 1ull << adr;
  if(s->inVblank || !s->renderFrame || s->refPpu || s->vPos == 0) {
    ppu_apply(s, adr, val);
    return;
  }
  if(same) return; /* games driving a register from HDMA often send the same value on every line */
  int due = snes_hPos(s) >= 512 ? s->vPos : s->vPos - 1;
  if(due > NS_SCREEN_H_OVERSCAN) due = NS_SCREEN_H_OVERSCAN;
  if(ppu->wlogCount == 0 && (kind == 2 || due <= ppu->renderedLine)) {
    ppu_apply(s, adr, val); /* nothing is waiting, and no line is due before it */
    return;
  }
  if(adr == 0x06) { /* mosaic: remembers the line it is written on */
    ppu_flush(s);
    ppu_apply(s, adr, val);
    return;
  }
  if(ppu->wlogCount == NS_WLOG) ppu_flush(s);
  ppu->wlog[ppu->wlogCount++] = ((uint32_t)due << 16) | ((uint32_t)adr << 8) | val;
}

''' + wr
regs += wr
regs = re.sub(r'      // fallthrough to normal layer BG-[HV]OFS\n    \}\n', '    }\n    /* fallthrough */\n', regs)
# nwsnes: keep the RGB565 palette and the visible sprite list up to date
old = '''        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;'''
new = '''        const uint16_t c = (uint16_t)((val << 8) | ppu->cgramBuffer);
        ppu->pal565[ppu->cgramPointer] = (uint16_t)(
          (ppu->lut5[c & 0x1f] << 11) | (ppu->lut6[(c >> 5) & 0x1f] << 5) | ppu->lut5[(c >> 10) & 0x1f]);
        ppu->cgram[ppu->cgramPointer++] = c;
        ppu->cgramGen++;'''
assert old in regs
regs = regs.replace(old, new)
for case in ('    case 0x01: {\n', '    case 0x04: {\n', '    case 0x33: {\n'):
    assert regs.count(case) >= 1, case
    i = regs.rindex(case)   # the one in ppu_write (ppu_read comes first)
    regs = regs[:i] + case + '      ppu->objListDirty = true;\n' + regs[i + len(case):]
regs = regs.replace('  ppu->lutBrightness = 0xff;\n', '  ppu->lutBrightness = 0xff;\n  ppu->objCount = 0;\n  ppu->objListDirty = true;\n')
regs = regs.replace('static uint16_t ppu_getVramRemap(Ppu* ppu) {', 'static uint16_t ppu_getVramRemap(const Ppu* ppu) {')
open(out + '/ppu_regs.c', 'w').write(regs)

# ----------------------------------------------------------------- ref --
ref = LICENSE % ('reference renderer (PC builds only)',
                 'This is LakeSnes\' accurate, slow, one-pixel-at-a-time renderer. nwsnes keeps\n'
                 ' * it to check its own fast line renderer against (see host/).')
ref += '''#ifdef NS_WITH_REFERENCE
#include <string.h>

#include "snes.h"

static Snes* R; /* instance being rendered */
static uint8_t refObjPixel[256];
static uint8_t refObjPrio[256];

'''
a = src.index('// array for layer definitions per mode')
b = src.index('static void ppu_handlePixel(Ppu* ppu, int x, int y);')
ref += src[a:b]
protos = '''static void ppu_handlePixel(Ppu* ppu, int x, int y);
static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b);
static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row);
static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, bool priority);
static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly);
static void ppu_calculateMode7Starts(Ppu* ppu, int y);
static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority);
static bool ppu_getWindowState(Ppu* ppu, int layer, int x);
static void ppu_evaluateSprites(Ppu* ppu, int line);

void ppu_refRunLine(Snes* s, int line) {
  Ppu* ppu = &s->ppu;
  R = s;
  ppu_buildLuts(ppu);
  // evaluate sprites
  memset(refObjPixel, 0, sizeof(refObjPixel));
  if(!ppu->forcedBlank) ppu_evaluateSprites(ppu, line - 1);
  // actual line
  if(ppu->mode == 7) ppu_calculateMode7Starts(ppu, line);
  for(int x = 0; x < 256; x++) {
    ppu_handlePixel(ppu, x, line);
  }
  s->putRows(s->user, line - 1, 1, ppu->band);
}

'''
ref += protos
hp = func('ppu_handlePixel')
# keep the colour maths, replace the 512-wide BGRX output by one RGB565 pixel
cutpos = hp.index('  int row = (y - 1) + (ppu->evenFrame ? 0 : 239);')
hp = hp[:cutpos] + '''  // nwsnes: 256 pixels wide, RGB565, main screen pixel only
  ppu->band[x] = (uint16_t)((ppu->lut5[r] << 11) | (ppu->lut6[g] << 5) | ppu->lut5[b]);
  (void) r2; (void) g2; (void) b2;
}
'''
ref += hp + '\n'
for name in ('ppu_getPixel', 'ppu_handleOPT', 'ppu_getOffsetValue', 'ppu_getPixelForBgLayer',
             'ppu_calculateMode7Starts', 'ppu_getPixelForMode7', 'ppu_getWindowState', 'ppu_evaluateSprites'):
    f = func(name)
    f = vram_reads(f, 'R')
    f = f.replace('ppu->objPixelBuffer', 'refObjPixel').replace('ppu->objPriorityBuffer', 'refObjPrio')
    # shifting a negative value left is undefined behaviour in C
    f = f.replace('(xCenter << 8)', '(xCenter * 256)').replace('(yCenter << 8)', '(yCenter * 256)')
    assert 'ppu->vram[' not in f and 'ppu->snes' not in f, name
    ref += f + '\n'
ref += '#endif /* NS_WITH_REFERENCE */\n'
open(out + '/ppu_ref.c', 'w').write(ref)
print('ok')
