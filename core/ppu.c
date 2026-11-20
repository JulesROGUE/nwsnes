/*
 * nwsnes - renderer
 *
 * There is no frame buffer: the picture is drawn in bands of NS_BAND lines
 * straight into a small RGB565 buffer that is handed to the display.
 *
 * It is also drawn late. Nothing is done when a scanline goes by; the lines
 * are drawn in one go when the frame ends, or earlier when the game reads
 * something back from the PPU. Register writes made in the meantime are
 * written down and replayed between the lines they separate (ppu_write in
 * ppu_regs.c, ppu_flush here). That way
 * the renderer and the cpu core run one after the other instead of taking
 * turns 224 times per frame, which matters on a processor with an 8 KB
 * instruction cache.
 *
 * Two renderers share the job:
 *
 *  - the fast path paints the layers of a band from back to front, tile by
 *    tile, directly as colours. It handles what games do most of the time:
 *    modes 0 and 1, 8x8 tiles, no window in use, no mosaic, and colour math
 *    limited to a fixed colour or to the backdrop;
 *  - everything else goes line by line through the general renderer, which
 *    keeps for every pixel its priority and layer (a "z-buffer" line) and
 *    resolves windows and colour math per pixel.
 *
 * The priority tables, the pixel-exact paths (mosaic, offset-per-tile,
 * hi-res) and the general semantics come from LakeSnes (c) 2021-2023
 * angelo_wf and contributors, MIT license; the z-buffer idea follows the
 * snesrev renderer (c) 2021-2023 snesrev / elzo_d, MIT license. Output is
 * checked against LakeSnes' renderer (ppu_ref.c) by the host tests.
 *
 * Not handled: direct colour, and 512-pixel modes are shown at 256 pixels.
 */
#include <string.h>

#include "snes.h"

#define Z_BACKDROP 0x0500u
#define LAYER_OBJ 4
#define LAYER_OBJ_NOMATH 6
#define LAYER_BACKDROP 5

/* per mode: layers from front to back (0-3: bg 1-4, 4: sprites, 5: none)... */
static const uint8_t layersPerMode[10][12] = {
  {4, 0, 1, 4, 0, 1, 4, 2, 3, 4, 2, 3},
  {4, 0, 1, 4, 0, 1, 4, 2, 4, 2, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5},
  {4, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5, 5},
  {2, 4, 0, 1, 4, 0, 1, 4, 4, 2, 5, 5}, /* 8: mode 1 with bg3 priority */
  {4, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5, 5}  /* 9: mode 7 with extbg */
};

/* ...and which priority of that layer the entry stands for */
static const uint8_t prioritysPerMode[10][12] = {
  {3, 1, 1, 2, 0, 0, 1, 1, 1, 0, 0, 0},
  {3, 1, 1, 2, 0, 0, 1, 1, 0, 0, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5},
  {3, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5, 5},
  {1, 3, 1, 1, 2, 0, 0, 1, 0, 0, 5, 5},
  {3, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5, 5}
};

static const uint8_t layerCountPerMode[10] = {
  12, 10, 8, 8, 8, 8, 6, 5, 10, 7
};

/* 5: layer does not exist in this mode; 7: mode 7 ext bg */
static const uint8_t bitDepthsPerMode[10][4] = {
  {2, 2, 2, 2},
  {4, 4, 2, 5},
  {4, 4, 5, 5},
  {8, 4, 5, 5},
  {8, 2, 5, 5},
  {4, 2, 5, 5},
  {4, 5, 5, 5},
  {8, 5, 5, 5},
  {4, 4, 2, 5},
  {8, 7, 5, 5}
};

static const uint8_t spriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

/* bit i of a nibble -> byte i of a word. kSpread[n] puts the nibble's most
 * significant bit in the first (leftmost) byte, kSpreadFlip the least. */
static const uint32_t kSpread[16] = {
  0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
  0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101
};
static const uint32_t kSpreadFlip[16] = {
  0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
  0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101
};

/* priority levels of the current mode, as z-buffer values (level << 12) */
typedef struct Levels {
  uint16_t bg[4][2]; /* [layer][tile priority] */
  uint16_t obj[4];   /* [sprite priority] */
  uint8_t actMode;
} Levels;

/* spans of a line where a layer is (or is not) hidden by the windows */
typedef struct Spans {
  uint8_t n;
  uint8_t masked;  /* bit i set: span i is hidden */
  int16_t edge[6]; /* span i is [edge[i], edge[i + 1]) */
} Spans;

void ppu_buildLuts(Ppu* ppu) {
  if(ppu->lutBrightness == ppu->brightness) return;
  ppu->lutBrightness = ppu->brightness;
  for(int c = 0; c < 32; c++) {
    int c8 = ((c << 3) | (c >> 2)) * ppu->brightness / 15;
    ppu->lut5[c] = (uint8_t)(c8 >> 3);
    ppu->lut6[c] = (uint8_t)(c8 >> 2);
  }
  for(int i = 0; i < 0x100; i++) {
    const uint16_t c = ppu->cgram[i];
    ppu->pal565[i] = (uint16_t)(
      (ppu->lut5[c & 0x1f] << 11) | (ppu->lut6[(c >> 5) & 0x1f] << 5) | ppu->lut5[(c >> 10) & 0x1f]);
  }
}

static void getLevels(const Ppu* ppu, Levels* lv) {
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  actMode = ppu->mode == 7 && ppu->m7extBg ? 9 : actMode;
  lv->actMode = (uint8_t)actMode;
  memset(lv->bg, 0, sizeof(lv->bg));
  memset(lv->obj, 0, sizeof(lv->obj));
  int count = layerCountPerMode[actMode];
  for(int i = 0; i < count; i++) {
    int layer = layersPerMode[actMode][i];
    int prio = prioritysPerMode[actMode][i];
    uint16_t z = (uint16_t)((count - i) << 12);
    if(layer < 4) lv->bg[layer][prio] = (uint16_t)(z | (layer << 8));
    else if(layer == 4) lv->obj[prio] = z;
  }
}

/* same truth table as LakeSnes' ppu_getWindowState */
static bool windowState(const Ppu* ppu, int layer, int x) {
  const WindowLayer* w = &ppu->windowLayer[layer];
  if(!w->window1enabled && !w->window2enabled) return false;
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if(w->window1enabled && !w->window2enabled) return w->window1inversed ? !test1 : test1;
  if(!w->window1enabled && w->window2enabled) return w->window2inversed ? !test2 : test2;
  if(w->window1inversed) test1 = !test1;
  if(w->window2inversed) test2 = !test2;
  switch(w->maskLogic) {
    case 0: return test1 || test2;
    case 1: return test1 && test2;
    case 2: return test1 != test2;
    default: return test1 == test2;
  }
}

/* cuts the line where the window state of this layer can change */
static void getSpans(const Ppu* ppu, int layer, bool windowed, Spans* sp) {
  const WindowLayer* w = &ppu->windowLayer[layer];
  sp->n = 1;
  sp->masked = 0;
  sp->edge[0] = 0;
  sp->edge[1] = NS_SCREEN_W;
  if(!windowed || (!w->window1enabled && !w->window2enabled)) return;
  int cuts[4];
  int nc = 0;
  if(w->window1enabled) {
    cuts[nc++] = ppu->window1left;
    cuts[nc++] = ppu->window1right + 1;
  }
  if(w->window2enabled) {
    cuts[nc++] = ppu->window2left;
    cuts[nc++] = ppu->window2right + 1;
  }
  /* insertion sort, at most 4 values */
  for(int i = 1; i < nc; i++) {
    int v = cuts[i], j = i - 1;
    while(j >= 0 && cuts[j] > v) {
      cuts[j + 1] = cuts[j];
      j--;
    }
    cuts[j + 1] = v;
  }
  int n = 0;
  for(int i = 0; i < nc; i++) {
    if(cuts[i] > sp->edge[n] && cuts[i] < NS_SCREEN_W) sp->edge[++n] = (int16_t)cuts[i];
  }
  sp->edge[++n] = NS_SCREEN_W;
  sp->n = (uint8_t)n;
  for(int i = 0; i < n; i++) {
    if(windowState(ppu, layer, sp->edge[i])) sp->masked |= (uint8_t)(1 << i);
  }
}

/* true when the whole line is on the same side of the windows; *hidden tells which */
static bool spansUniform(const Spans* sp, bool* hidden) {
  const uint32_t all = (1u << sp->n) - 1;
  *hidden = sp->masked != 0;
  return sp->masked == 0 || sp->masked == all;
}

/* ------------------------------------------------------ backgrounds -- */

/* Start of a tile's pixel data. Tiles are 16, 32 or 64 bytes and aligned on
 * their size, so a tile never straddles two 256-byte pages. */
static inline const uint8_t* tileData(const Snes* s, uint32_t wordAdr) {
  wordAdr &= 0x7fff;
  return s->pager.ptr[NS_VP_VRAM + (wordAdr >> 7)] + ((wordAdr & 0x7f) << 1);
}

/* one row of a tile -> 8 palette-relative pixels, left to right.
 * Returns false (px untouched) when the whole row is transparent. */
static inline bool expandRow(const uint8_t* tile, int row, int bpp, bool hflip, uint8_t px[8]) {
  const uint32_t* hi = hflip ? kSpreadFlip : kSpread;
  const uint8_t* p = tile + row * 2;
  uint32_t any = p[0] | p[1];
  if(bpp > 2) any |= p[16] | p[17];
  if(bpp > 4) any |= p[32] | p[33] | p[48] | p[49];
  if(!any) return false;
  uint32_t l = 0, r = 0; /* left and right 4 pixels */
  for(int plane = 0; plane < bpp; plane += 2, p += 16) {
    uint32_t b0 = p[0], b1 = p[1];
    if(hflip) {
      l |= (hi[b0 & 15] | (hi[b1 & 15] << 1)) << plane;
      r |= (hi[b0 >> 4] | (hi[b1 >> 4] << 1)) << plane;
    } else {
      l |= (hi[b0 >> 4] | (hi[b1 >> 4] << 1)) << plane;
      r |= (hi[b0 & 15] | (hi[b1 & 15] << 1)) << plane;
    }
  }
  px[0] = (uint8_t)l; px[1] = (uint8_t)(l >> 8); px[2] = (uint8_t)(l >> 16); px[3] = (uint8_t)(l >> 24);
  px[4] = (uint8_t)r; px[5] = (uint8_t)(r >> 8); px[6] = (uint8_t)(r >> 16); px[7] = (uint8_t)(r >> 24);
  return true;
}

/* The common case: 8x8 or 16x16 tiles, no mosaic, no offset-per-tile. */
static void drawBg(Snes* s, int y, uint16_t* dst, int layer, int bpp, const Levels* lv, const Spans* sp) {
  const Ppu* ppu = &s->ppu;
  const BgLayer* bg = &ppu->bgLayer[layer];
  const bool big = bg->bigTiles;
  const int tb = big ? 4 : 3;
  const uint32_t highBit = big ? 0x200 : 0x100;
  const uint32_t ly = (uint32_t)(y + bg->vScroll) & 0x3ff;
  uint32_t rowAdr = bg->tilemapAdr + (((ly >> tb) & 0x1f) << 5);
  if((ly & highBit) && bg->tilemapHigher) rowAdr += bg->tilemapWider ? 0x800 : 0x400;
  const int paletteBase = (ppu->mode == 0) ? 32 * layer : 0;
  const int paletteShift = bpp == 2 ? 2 : (bpp == 4 ? 4 : 8);
  const uint32_t tileWords = (uint32_t)bpp * 4;
  const uint16_t zlo = lv->bg[layer][0], zhi = lv->bg[layer][1];

  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    int x = sp->edge[i];
    const int end = sp->edge[i + 1];
    while(x < end) {
      const uint32_t lx = (uint32_t)(x + bg->hScroll) & 0x3ff;
      const int first = (int)(lx & 7);
      int count = 8 - first;
      if(count > end - x) count = end - x;
      uint32_t mapAdr = rowAdr + ((lx >> tb) & 0x1f);
      if((lx & highBit) && bg->tilemapWider) mapAdr += 0x400;
      const uint32_t tile = snes_vramRead(s, mapAdr);
      uint32_t tileNum = tile & 0x3ff;
      if(big) {
        if(((lx & 8) != 0) != ((tile & 0x4000) != 0)) tileNum += 1;
        if(((ly & 8) != 0) != ((tile & 0x8000) != 0)) tileNum += 0x10;
        tileNum &= 0x3ff;
      }
      const int row = (tile & 0x8000) ? 7 - (int)(ly & 7) : (int)(ly & 7);
      const uint8_t* data = tileData(s, bg->tileAdr + tileNum * tileWords);
      uint8_t px[8];
      if(expandRow(data, row, bpp, (tile & 0x4000) != 0, px)) {
        const uint16_t z = (tile & 0x2000) ? zhi : zlo;
        const uint32_t pal = (uint32_t)(paletteBase + (int)(((tile & 0x1c00) >> 10) << paletteShift));
        uint16_t* d = dst + x;
        for(int k = 0; k < count; k++) {
          const uint8_t p = px[first + k];
          if(p && z > d[k]) d[k] = (uint16_t)(z | ((pal + p) & 0xff));
        }
      }
      x += count;
    }
  }
}

/* ---- pixel-exact slow path (mosaic, offset-per-tile, hi-res modes) ---- */

static uint16_t getOffsetValue(const Snes* s, int col, int row) {
  const BgLayer* bg = &s->ppu.bgLayer[2];
  int x = col * 8 + bg->hScroll;
  int y = row * 8 + bg->vScroll;
  int tileBits = bg->bigTiles ? 4 : 3;
  int tileHighBit = bg->bigTiles ? 0x200 : 0x100;
  uint32_t tilemapAdr = bg->tilemapAdr + (uint32_t)(((y >> tileBits) & 0x1f) << 5 | ((x >> tileBits) & 0x1f));
  if((x & tileHighBit) && bg->tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBit) && bg->tilemapHigher) tilemapAdr += bg->tilemapWider ? 0x800 : 0x400;
  return snes_vramRead(s, tilemapAdr);
}

static void handleOPT(const Snes* s, int layer, int* lx, int* ly) {
  const Ppu* ppu = &s->ppu;
  int x = *lx;
  int y = *ly;
  int column = 0;
  if(ppu->mode == 6) {
    column = ((x - (x & 0xf)) - ((ppu->bgLayer[layer].hScroll * 2) & 0xfff0)) >> 4;
  } else {
    column = ((x - (x & 0x7)) - (ppu->bgLayer[layer].hScroll & 0xfff8)) >> 3;
  }
  if(column > 0) {
    /* fetch offset values from layer 3 tilemap */
    int valid = layer == 0 ? 0x2000 : 0x4000;
    uint16_t hOffset = getOffsetValue(s, column - 1, 0);
    uint16_t vOffset = 0;
    if(ppu->mode == 4) {
      if(hOffset & 0x8000) {
        vOffset = hOffset;
        hOffset = 0;
      }
    } else {
      vOffset = getOffsetValue(s, column - 1, 1);
    }
    if(ppu->mode == 6) {
      if(hOffset & valid) *lx = (((hOffset & 0x3f8) + (column * 8)) * 2) | (x & 0xf);
    } else {
      if(hOffset & valid) *lx = ((hOffset & 0x3f8) + (column * 8)) | (x & 0x7);
    }
    if(vOffset & valid) *ly = (vOffset & 0x3ff) + (y - ppu->bgLayer[layer].vScroll);
  }
}

/* returns the palette index (0: transparent) and the tile's priority bit */
static int bgPixel(const Snes* s, int x, int y, int layer, int bitDepth, bool* priority) {
  const Ppu* ppu = &s->ppu;
  const BgLayer* bg = &ppu->bgLayer[layer];
  bool wideTiles = bg->bigTiles || ppu->mode == 5 || ppu->mode == 6;
  int tileBitsX = wideTiles ? 4 : 3;
  int tileHighBitX = wideTiles ? 0x200 : 0x100;
  int tileBitsY = bg->bigTiles ? 4 : 3;
  int tileHighBitY = bg->bigTiles ? 0x200 : 0x100;
  uint32_t tilemapAdr = bg->tilemapAdr + (uint32_t)(((y >> tileBitsY) & 0x1f) << 5 | ((x >> tileBitsX) & 0x1f));
  if((x & tileHighBitX) && bg->tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBitY) && bg->tilemapHigher) tilemapAdr += bg->tilemapWider ? 0x800 : 0x400;
  uint16_t tile = snes_vramRead(s, tilemapAdr);
  *priority = (tile & 0x2000) != 0;
  int paletteNum = (tile & 0x1c00) >> 10;
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int col = (tile & 0x4000) ? (x & 0x7) : 7 - (x & 0x7);
  int tileNum = tile & 0x3ff;
  if(wideTiles) {
    if(((bool)(x & 8)) ^ ((bool)(tile & 0x4000))) tileNum += 1;
  }
  if(bg->bigTiles) {
    if(((bool)(y & 8)) ^ ((bool)(tile & 0x8000))) tileNum += 0x10;
  }
  if(ppu->mode == 0) paletteNum += 8 * layer;
  uint32_t base = bg->tileAdr + (uint32_t)((tileNum & 0x3ff) * 4 * bitDepth) + (uint32_t)row;
  int paletteSize = 4;
  uint16_t plane1 = snes_vramRead(s, base);
  int pixel = (plane1 >> col) & 1;
  pixel |= ((plane1 >> (8 + col)) & 1) << 1;
  if(bitDepth > 2) {
    paletteSize = 16;
    uint16_t plane2 = snes_vramRead(s, base + 8);
    pixel |= ((plane2 >> col) & 1) << 2;
    pixel |= ((plane2 >> (8 + col)) & 1) << 3;
  }
  if(bitDepth > 4) {
    paletteSize = 256;
    uint16_t plane3 = snes_vramRead(s, base + 16);
    pixel |= ((plane3 >> col) & 1) << 4;
    pixel |= ((plane3 >> (8 + col)) & 1) << 5;
    uint16_t plane4 = snes_vramRead(s, base + 24);
    pixel |= ((plane4 >> col) & 1) << 6;
    pixel |= ((plane4 >> (8 + col)) & 1) << 7;
  }
  return pixel == 0 ? 0 : ((paletteSize * paletteNum + pixel) & 0xff);
}

static void drawBgSlow(Snes* s, int y, bool sub, uint16_t* dst, int layer, int bpp, const Levels* lv, const Spans* sp) {
  const Ppu* ppu = &s->ppu;
  const BgLayer* bg = &ppu->bgLayer[layer];
  const bool mosaic = bg->mosaicEnabled && ppu->mosaicSize > 1;
  /* with mosaic, neighbouring pixels show the same source pixel: fetch it once */
  int lastLx = -1;
  int pixel = 0;
  bool prio = false;
  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    for(int x = sp->edge[i]; x < sp->edge[i + 1]; x++) {
      int lx = x;
      int ly = y;
      if(mosaic) {
        lx -= lx % ppu->mosaicSize;
        ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
      }
      if(lx != lastLx) {
        lastLx = lx;
        lx += bg->hScroll;
        if(ppu->mode == 5 || ppu->mode == 6) {
          lx *= 2;
          lx += (sub || bg->mosaicEnabled) ? 0 : 1;
          if(ppu->interlace) {
            ly *= 2;
            ly += (ppu->evenFrame || bg->mosaicEnabled) ? 0 : 1;
          }
        }
        ly += bg->vScroll;
        if(ppu->mode == 2 || ppu->mode == 4 || ppu->mode == 6) handleOPT(s, layer, &lx, &ly);
        pixel = bgPixel(s, lx & 0x3ff, ly & 0x3ff, layer, bpp, &prio);
      }
      if(pixel) {
        uint16_t z = lv->bg[layer][prio];
        if(z > dst[x]) dst[x] = (uint16_t)(z | pixel);
      }
    }
  }
}

/* ------------------------------------------------------------ mode 7 -- */

static void calculateMode7Starts(Ppu* ppu, int y) {
  /* expand 13-bit values to signed values */
  int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  if(ppu->bgLayer[0].mosaicEnabled && ppu->mosaicSize > 1) {
    y -= (y - ppu->mosaicStartLine) % ppu->mosaicSize;
  }
  uint8_t ry = (uint8_t)(ppu->m7yFlip ? 255 - y : y);
  ppu->m7startX = (
    ((ppu->m7matrix[0] * clippedH) & ~63) +
    ((ppu->m7matrix[1] * ry) & ~63) +
    ((ppu->m7matrix[1] * clippedV) & ~63) +
    (xCenter * 256)
  );
  ppu->m7startY = (
    ((ppu->m7matrix[2] * clippedH) & ~63) +
    ((ppu->m7matrix[3] * ry) & ~63) +
    ((ppu->m7matrix[3] * clippedV) & ~63) +
    (yCenter * 256)
  );
}

/* layer 0: the 8-bit plane. layer 1 (extbg): 7 bits, top bit is priority */
static void drawMode7(Snes* s, uint16_t* dst, int layer, const Levels* lv, const Spans* sp) {
  const Ppu* ppu = &s->ppu;
  const bool mosaic = ppu->bgLayer[layer].mosaicEnabled && ppu->mosaicSize > 1;
  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    for(int x = sp->edge[i]; x < sp->edge[i + 1]; x++) {
      int lx = x;
      if(mosaic) lx -= lx % ppu->mosaicSize;
      uint8_t rx = (uint8_t)(ppu->m7xFlip ? 255 - lx : lx);
      int xPos = (ppu->m7startX + ppu->m7matrix[0] * rx) >> 8;
      int yPos = (ppu->m7startY + ppu->m7matrix[2] * rx) >> 8;
      bool outsideMap = xPos < 0 || xPos >= 1024 || yPos < 0 || yPos >= 1024;
      xPos &= 0x3ff;
      yPos &= 0x3ff;
      if(!ppu->m7largeField) outsideMap = false;
      uint8_t tile = outsideMap ? 0 : (uint8_t)(snes_vramRead(s, (uint32_t)((yPos >> 3) * 128 + (xPos >> 3))) & 0xff);
      uint8_t pixel = outsideMap && !ppu->m7charFill ? 0 :
        (uint8_t)(snes_vramRead(s, (uint32_t)(tile * 64 + (yPos & 7) * 8 + (xPos & 7))) >> 8);
      uint16_t z;
      if(layer == 1) {
        z = lv->bg[1][pixel >> 7];
        pixel &= 0x7f;
      } else {
        z = lv->bg[0][0];
      }
      if(pixel && z > dst[x]) dst[x] = (uint16_t)(z | pixel);
    }
  }
}

/* ----------------------------------------------------------- sprites -- */

#define OBJ_PIXEL 0x8000u

/* Lists the sprites that are on screen on at least one line, in the order
 * the hardware scans them (from "start", wrapping around). */
static void buildObjList(Ppu* ppu, uint8_t start) {
  int n = 0;
  uint8_t index = start;
  for(int i = 0; i < 128; i++, index = (uint8_t)(index + 2)) {
    const int y = ppu->oam[index] >> 8;
    const int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    const int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    /* rows wrap at 256: a sprite low enough comes back in at the top */
    if(y >= NS_SCREEN_H_OVERSCAN && y + spriteHeight <= 256) continue;
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x > 255) x -= 512;
    if(x <= -spriteSize) continue;
    ppu->objList[n++] = index;
  }
  ppu->objCount = (uint8_t)n;
  ppu->objListStart = start;
  ppu->objListDirty = false;
}

/* Fills row r of the band with the sprite pixels of one line. Same selection
 * rules as LakeSnes: at most 32 sprites and 34 tiles per line, later sprites
 * are dropped first, and among sprites the first one in the scan wins
 * whatever its priority. */
static void evalSprites(Snes* s, int line, int r) {
  Ppu* ppu = &s->ppu;
  uint16_t* buf = ppu->objRow[r];
  /* erase what the last use of this row left */
  for(int i = 0; i < ppu->objSlivers[r]; i++) {
    int x = ppu->objSliverX[r][i];
    int n = 8;
    if(x < 0) {
      n += x;
      x = 0;
    }
    if(x + n > NS_SCREEN_W) n = NS_SCREEN_W - x;
    memset(buf + x, 0, (size_t)n * sizeof(uint16_t));
  }
  int slivers = 0;
  uint32_t prioMask = 0;
  /* with priority rotation the scan starts at the sprite OAM points to */
  const uint8_t start = ppu->objPriority ? (ppu->oamAdr & 0xfe) : 0;
  if(ppu->objListDirty || start != ppu->objListStart) buildObjList(ppu, start);
  int spritesFound = 0;
  int tilesFound = 0;
  uint8_t foundSprites[32];
  for(int i = 0; i < ppu->objCount; i++) {
    const uint8_t index = ppu->objList[i];
    uint8_t y = (uint8_t)(ppu->oam[index] >> 8);
    uint8_t row = (uint8_t)(line - y);
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    if(row < spriteHeight) {
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      if(x > 255) x -= 512;
      if(x > -spriteSize) {
        spritesFound++;
        if(spritesFound > 32) {
          ppu->rangeOver = true;
          spritesFound = 32;
          break;
        }
        foundSprites[spritesFound - 1] = index;
      }
    }
  }
  /* iterate over found sprites backwards to fetch max 34 tile slivers */
  for(int i = spritesFound; i > 0; i--) {
    const uint8_t index = foundSprites[i - 1];
    uint8_t y = (uint8_t)(ppu->oam[index] >> 8);
    uint8_t row = (uint8_t)(line - y);
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x > 255) x -= 512;
    if(x <= -spriteSize) continue;
    if(ppu->objInterlace) row = (uint8_t)(row * 2 + (ppu->evenFrame ? 0 : 1));
    const uint16_t attr = ppu->oam[index + 1];
    const int tile = attr & 0xff;
    const bool hFlipped = attr & 0x4000;
    if(attr & 0x8000) row = (uint8_t)(spriteSize - 1 - row);
    const uint32_t prio = (attr & 0x3000) >> 12;
    const uint32_t value = OBJ_PIXEL | (prio << 8) | (0x80u + 16u * ((attr & 0xe00) >> 9));
    const uint32_t objAdr = (attr & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
    for(int col = 0; col < spriteSize; col += 8) {
      if(col + x > -8 && col + x < 256) {
        tilesFound++;
        if(tilesFound > 34) {
          ppu->timeOver = true;
          break;
        }
        /* which tile this is, looping within 16x16 pages */
        int usedCol = hFlipped ? spriteSize - 1 - col : col;
        uint8_t usedTile = (uint8_t)((((tile >> 4) + (row / 8)) << 4) | (((tile & 0xf) + (usedCol / 8)) & 0xf));
        const uint8_t* data = tileData(s, objAdr + (uint32_t)usedTile * 16);
        uint8_t px[8];
        if(expandRow(data, row & 7, 4, hFlipped, px)) {
          int from = col + x < 0 ? -(col + x) : 0;
          int to = col + x > 248 ? 256 - (col + x) : 8;
          ppu->objSliverX[r][slivers++] = (int16_t)(col + x);
          prioMask |= 1u << prio;
          for(int k = from; k < to; k++) {
            if(px[k]) buf[col + x + k] = (uint16_t)(value + px[k]);
          }
        }
      }
    }
    if(tilesFound > 34) break;
  }
  ppu->objSlivers[r] = (uint8_t)slivers;
  ppu->objPrioMask[r] = (uint8_t)prioMask;
}

/* general renderer: sprites of row r into a z-buffer line */
static void mergeSprites(const Ppu* ppu, uint16_t* dst, const Spans* sp, const Levels* lv, int r) {
  const uint16_t* buf = ppu->objRow[r];
  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    for(int t = 0; t < ppu->objSlivers[r]; t++) {
      int x = ppu->objSliverX[r][t];
      int end = x + 8;
      if(x < sp->edge[i]) x = sp->edge[i];
      if(end > sp->edge[i + 1]) end = sp->edge[i + 1];
      for(; x < end; x++) {
        const uint32_t o = buf[x];
        if(!o) continue;
        /* palettes 4-7 take part in colour math, 0-3 do not */
        const uint16_t z = (uint16_t)(lv->obj[(o >> 8) & 3] | (((o & 0xc0) == 0xc0 ? LAYER_OBJ : LAYER_OBJ_NOMATH) << 8) | (o & 0xff));
        if(z > dst[x]) dst[x] = z;
      }
    }
  }
}

/* ------------------------------------------- general renderer, a line -- */

static void drawScreen(Snes* s, int y, bool sub, const Levels* lv, int r) {
  Ppu* ppu = &s->ppu;
  uint16_t* dst = ppu->zbuf[sub];
  for(int x = 0; x < NS_SCREEN_W; x++) dst[x] = Z_BACKDROP;
  Spans sp;
  for(int layer = 0; layer < 4; layer++) {
    int bpp = bitDepthsPerMode[lv->actMode][layer];
    if(bpp == 5) continue;
    const Layer* l = &ppu->layer[layer];
    if(!(sub ? l->subScreenEnabled : l->mainScreenEnabled)) continue;
    getSpans(ppu, layer, sub ? l->subScreenWindowed : l->mainScreenWindowed, &sp);
    if(ppu->mode == 7) {
      drawMode7(s, dst, layer, lv, &sp);
    } else if((ppu->bgLayer[layer].mosaicEnabled && ppu->mosaicSize > 1) || ppu->mode == 2 || ppu->mode >= 4) {
      drawBgSlow(s, y, sub, dst, layer, bpp, lv, &sp);
    } else {
      drawBg(s, y, dst, layer, bpp, lv, &sp);
    }
  }
  if(ppu->objSlivers[r]) {
    const Layer* l = &ppu->layer[4];
    if(sub ? l->subScreenEnabled : l->mainScreenEnabled) {
      getSpans(ppu, 4, sub ? l->subScreenWindowed : l->mainScreenWindowed, &sp);
      mergeSprites(ppu, dst, &sp, lv, r);
    }
  }
}

/* colour math of one pixel: c is the main screen colour (or black when
 * clipped), c2 what it is mixed with; both and the result are 15-bit BGR */
static inline uint16_t blend565(const Ppu* ppu, uint32_t c, uint32_t c2, bool half) {
  int r = (int)(c & 0x1f), g = (int)((c >> 5) & 0x1f), b = (int)((c >> 10) & 0x1f);
  const int r2 = (int)(c2 & 0x1f), g2 = (int)((c2 >> 5) & 0x1f), b2 = (int)((c2 >> 10) & 0x1f);
  if(ppu->subtractColor) {
    r -= r2; g -= g2; b -= b2;
  } else {
    r += r2; g += g2; b += b2;
  }
  if(half) {
    r >>= 1; g >>= 1; b >>= 1;
  }
  if(r > 31) r = 31;
  if(g > 31) g = 31;
  if(b > 31) b = 31;
  if(r < 0) r = 0;
  if(g < 0) g = 0;
  if(b < 0) b = 0;
  return (uint16_t)((ppu->lut5[r] << 11) | (ppu->lut6[g] << 5) | ppu->lut5[b]);
}

static inline uint32_t fixedColor(const Ppu* ppu) {
  return (uint32_t)(ppu->fixedColorR | (ppu->fixedColorG << 5) | (ppu->fixedColorB << 10));
}

/* line: 1..224/239; r: its row in the band; out: 256 pixels */
static void slowLine(Snes* s, int line, int r, uint16_t* out) {
  Ppu* ppu = &s->ppu;
  Levels lv;
  getLevels(ppu, &lv);
  if(ppu->mode == 7) calculateMode7Starts(ppu, line);

  drawScreen(s, line, false, &lv, r);

  /* which layer codes take part in colour math (code 6, sprites with a
   * palette below 4, never does) */
  uint32_t mathMask = 0;
  for(int i = 0; i < 6; i++) mathMask |= (uint32_t)ppu->mathEnabled[i] << i;
  const bool useSub = mathMask && ppu->addSubscreen && ppu->preventMathMode != 3;
  if(useSub) drawScreen(s, line, true, &lv, r);

  const uint16_t* zm = ppu->zbuf[0];
  const uint16_t* zs = ppu->zbuf[1];
  const uint16_t* cgram = ppu->cgram;
  const uint16_t* pal565 = ppu->pal565;
  const uint32_t fixed = fixedColor(ppu);
  Spans cw;
  getSpans(ppu, 5, true, &cw);
  for(int i = 0; i < cw.n; i++) {
    const bool inWindow = cw.masked & (1 << i);
    const bool clip = ppu->clipMode == 3 || (ppu->clipMode == 2 && inWindow) || (ppu->clipMode == 1 && !inWindow);
    const bool mathAllowed = mathMask && !(
      ppu->preventMathMode == 3 || (ppu->preventMathMode == 2 && inWindow) || (ppu->preventMathMode == 1 && !inWindow)
    );
    int x = cw.edge[i];
    const int end = cw.edge[i + 1];
    if(!mathAllowed) {
      if(clip) {
        for(; x < end; x++) out[x] = 0;
      } else {
        for(; x < end; x++) out[x] = pal565[zm[x] & 0xff];
      }
      continue;
    }
    /* blending is the expensive part, and neighbouring pixels are very often
     * the same colours (sky, flat areas): keep the last result at hand */
    uint32_t lastKey = 0xffffffffu;
    uint16_t lastOut = 0;
    for(; x < end; x++) {
      const uint16_t z = zm[x];
      if(!((mathMask >> ((z >> 8) & 0xf)) & 1)) {
        out[x] = clip ? 0 : pal565[z & 0xff];
        continue;
      }
      const uint32_t key = (z & 0xffu) | (ppu->addSubscreen ? (uint32_t)zs[x] << 8 : 0u);
      if(key == lastKey) {
        out[x] = lastOut;
        continue;
      }
      lastKey = key;
      uint32_t c2 = fixed;
      bool half = ppu->halfColor;
      if(ppu->addSubscreen) {
        const uint16_t z2 = zs[x];
        if(((z2 >> 8) & 0xf) != LAYER_BACKDROP) c2 = cgram[z2 & 0xff];
        else half = false; /* the fixed colour standing in for the sub screen is never halved */
      }
      lastOut = blend565(ppu, clip ? 0 : cgram[z & 0xff], c2, half);
      out[x] = lastOut;
    }
  }
}

/* ------------------------------------------------- fast path, a band -- */

/* What a band is made of when the fast path applies: the colour of the
 * backdrop, then the layers to paint over it from back to front. */
/* why a band went to the general renderer (statistics, see Ppu.statWhy) */
enum { WHY_MODE = 1, WHY_COLOR_WINDOW, WHY_CLIP, WHY_TILES, WHY_WINDOW, WHY_MATH };

enum {
  BLEND_NONE = 0, /* plain colours */
  BLEND_PAL = 1,  /* the blended palette (sprites: palettes 4-7 only) */
  BLEND_ALL = 2,  /* the blended palette, sprites of all palettes */
  BLEND_SUB = 3   /* sprites of palettes 4-7 are mixed with the sub screen pixel under them */
};

typedef struct Pass {
  uint8_t layer; /* 0-3: background, 4: sprites */
  uint8_t prio;  /* tile priority bit (backgrounds) or sprite priority */
  uint8_t blend; /* BLEND_* */
} Pass;

typedef struct Plan {
  uint16_t backdrop; /* RGB565 */
  uint8_t n;
  uint8_t mode;
  uint8_t subShown; /* bit l: layer l (4: sprites) is part of the sub screen */
  Pass pass[24];
} Plan;

/* Which layers of one screen (main or sub) are shown; false when the fast
 * path cannot draw one of them. A window that hides a layer on the whole
 * line, or nowhere, is fine: games leave them set up that way a lot. */
static bool screenLayers(Ppu* ppu, int actMode, bool sub, uint32_t* shown) {
  *shown = 0;
  for(int layer = 0; layer < 5; layer++) {
    const Layer* l = &ppu->layer[layer];
    if(!(sub ? l->subScreenEnabled : l->mainScreenEnabled)) continue;
    if(layer < 4 && bitDepthsPerMode[actMode][layer] == 5) continue;
    Spans sp;
    getSpans(ppu, layer, sub ? l->subScreenWindowed : l->mainScreenWindowed, &sp);
    bool hidden;
    if(!spansUniform(&sp, &hidden)) {
      ppu->why = WHY_WINDOW;
      return false;
    }
    if(hidden) continue; /* everywhere */
    if(layer < 4) {
      const BgLayer* bg = &ppu->bgLayer[layer];
      if(bg->bigTiles || (bg->mosaicEnabled && ppu->mosaicSize > 1)) {
        ppu->why = WHY_TILES;
        return false;
      }
    }
    *shown |= 1u << layer;
  }
  return true;
}

/* appends the shown layers of one screen, back to front */
static void planScreen(Plan* plan, int actMode, uint32_t shown, bool sub, uint32_t mathMask, int objBlend) {
  for(int i = layerCountPerMode[actMode] - 1; i >= 0; i--) {
    const int layer = layersPerMode[actMode][i];
    if(layer > 4 || !((shown >> layer) & 1)) continue;
    Pass* p = &plan->pass[plan->n++];
    p->layer = (uint8_t)layer;
    p->prio = prioritysPerMode[actMode][i];
    if(sub) p->blend = BLEND_ALL;
    else if(!((mathMask >> layer) & 1)) p->blend = BLEND_NONE;
    else p->blend = (uint8_t)(layer == 4 ? objBlend : BLEND_PAL);
  }
}

/* Decides whether the current register state can be drawn by the fast path
 * and, if so, how. Colour math is only taken when it boils down to using
 * other colours:
 *  - against the fixed colour: a layer that takes part is painted with a
 *    palette blended once with that colour;
 *  - against the sub screen when only the backdrop takes part (a common way
 *    to show a background layer): where the main screen shows a layer the
 *    sub screen does not matter, elsewhere the result is the sub screen
 *    blended with the backdrop colour. So the sub screen layers are painted
 *    first, with a palette blended with the backdrop, then the main ones;
 *  - same when sprites take part too: their pixels are then mixed one by
 *    one with the sub screen pixel under them (see subPixel). */
static bool planBuild(Snes* s, Plan* plan) {
  Ppu* ppu = &s->ppu;
  ppu->why = WHY_MODE;
  if(ppu->mode > 1) return false;
  /* the colour window: fine as long as the whole line is on one side of it */
  Spans cw;
  getSpans(ppu, 5, true, &cw);
  ppu->why = WHY_COLOR_WINDOW;
  bool inWindow;
  if(!spansUniform(&cw, &inWindow)) return false;
  ppu->why = WHY_CLIP;
  if(ppu->clipMode == 3 || (ppu->clipMode == 2 && inWindow) || (ppu->clipMode == 1 && !inWindow)) return false;
  uint32_t mathMask = 0;
  for(int i = 0; i < 6; i++) mathMask |= (uint32_t)ppu->mathEnabled[i] << i;
  if(ppu->preventMathMode == 3 || (ppu->preventMathMode == 2 && inWindow) || (ppu->preventMathMode == 1 && !inWindow)) mathMask = 0;
  const int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  plan->n = 0;
  plan->mode = (uint8_t)actMode;
  plan->subShown = 0;

  uint32_t mainShown, subShown = 0;
  if(!screenLayers(ppu, actMode, false, &mainShown)) return false;
  /* layers that are not shown cannot take part in anything */
  mathMask &= mainShown | 0x20;
  if(mathMask && ppu->addSubscreen && !screenLayers(ppu, actMode, true, &subShown)) return false;

  const uint32_t c0 = ppu->cgram[0];
  const uint32_t fixed = fixedColor(ppu);
  uint32_t key[3];
  if(!mathMask) {
    plan->backdrop = ppu->pal565[0];
    planScreen(plan, actMode, mainShown, false, 0, BLEND_NONE);
    return true;
  }
  if(!subShown) {
    /* everything that takes part is mixed with the fixed colour (never
     * halved when it stands in for an empty sub screen) */
    const bool half = ppu->halfColor && !ppu->addSubscreen;
    plan->backdrop = (mathMask & 0x20) ? blend565(ppu, c0, fixed, half) : ppu->pal565[0];
    planScreen(plan, actMode, mainShown, false, mathMask, BLEND_PAL);
    if(!(mathMask & 0x1f)) return true; /* only the backdrop: no palette needed */
    key[0] = 0x10000u | fixed;
    key[1] = ppu->cgramGen;
    key[2] = (uint32_t)ppu->subtractColor | ((uint32_t)half << 1) | ((uint32_t)ppu->brightness << 8);
    if(memcmp(ppu->palBlendKey, key, sizeof(key)) != 0) {
      for(int i = 0; i < 0x100; i++) ppu->palBlend[i] = blend565(ppu, ppu->cgram[i], fixed, half);
      memcpy(ppu->palBlendKey, key, sizeof(key));
    }
    return true;
  }
  /* against the sub screen: backdrop and sprites only */
  ppu->why = WHY_MATH;
  if(mathMask & 0x0f) return false;
  plan->subShown = (uint8_t)subShown;
  if(mathMask & 0x20) {
    plan->backdrop = blend565(ppu, c0, fixed, false);
    planScreen(plan, actMode, subShown, true, 0, BLEND_NONE);
    key[0] = 0x20000u | c0;
    key[1] = ppu->cgramGen;
    key[2] = (uint32_t)ppu->subtractColor | ((uint32_t)ppu->halfColor << 1) | ((uint32_t)ppu->brightness << 8);
    if(memcmp(ppu->palBlendKey, key, sizeof(key)) != 0) {
      for(int i = 0; i < 0x100; i++) ppu->palBlend[i] = blend565(ppu, c0, ppu->cgram[i], ppu->halfColor);
      memcpy(ppu->palBlendKey, key, sizeof(key));
    }
  } else {
    plan->backdrop = ppu->pal565[0];
  }
  planScreen(plan, actMode, mainShown, false, mathMask, BLEND_SUB);
  return true;
}

/* What the sub screen shows at one pixel: its cgram index, or -1 for
 * nothing (the fixed colour then stands in). Slow, pixel-exact; only used
 * for the few sprite pixels that are mixed with the sub screen. */
static int subPixel(const Snes* s, const Plan* plan, int x, int line, int r) {
  const Ppu* ppu = &s->ppu;
  int bgIdx[4];
  bool bgPrio[4] = {false, false, false, false};
  uint32_t fetched = 0;
  for(int i = 0; i < layerCountPerMode[plan->mode]; i++) {
    const int layer = layersPerMode[plan->mode][i];
    if(layer > 4 || !((plan->subShown >> layer) & 1)) continue;
    const int prio = prioritysPerMode[plan->mode][i];
    if(layer == 4) {
      const uint32_t o = ppu->objRow[r][x];
      if(o && (int)((o >> 8) & 3) == prio) return (int)(o & 0xff);
      continue;
    }
    if(!((fetched >> layer) & 1)) {
      const BgLayer* bg = &ppu->bgLayer[layer];
      bgIdx[layer] = bgPixel(s, (x + bg->hScroll) & 0x3ff, (line + bg->vScroll) & 0x3ff, layer,
                             bitDepthsPerMode[plan->mode][layer], &bgPrio[layer]);
      fetched |= 1u << layer;
    }
    if(bgIdx[layer] && (int)bgPrio[layer] == prio) return bgIdx[layer];
  }
  return -1;
}

static inline uint32_t load32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

/* 8 pixels of a tile row, 4 bits each (0: transparent), leftmost in the low bits */
static inline void paint8(uint16_t* d, uint32_t w, const uint16_t* pal) {
  uint32_t p;
  if((p = w & 15) != 0) d[0] = pal[p];
  if((p = (w >> 4) & 15) != 0) d[1] = pal[p];
  if((p = (w >> 8) & 15) != 0) d[2] = pal[p];
  if((p = (w >> 12) & 15) != 0) d[3] = pal[p];
  if((p = (w >> 16) & 15) != 0) d[4] = pal[p];
  if((p = (w >> 20) & 15) != 0) d[5] = pal[p];
  if((p = (w >> 24) & 15) != 0) d[6] = pal[p];
  if((p = w >> 28) != 0) d[7] = pal[p];
}

/* same, for a tile that sticks out of the screen */
static void paintClipped(uint16_t* row, int x, uint32_t w, const uint16_t* pal) {
  for(int k = 0; k < 8; k++, w >>= 4) {
    const uint32_t p = w & 15;
    if(p && (unsigned)(x + k) < NS_SCREEN_W) row[x + k] = pal[p];
  }
}

/* Paints the tiles of one background layer that have the given priority
 * bit, over n lines starting at line y0. Each tile is looked up once for all
 * the lines it covers. bpp: 2 or 4. */
static inline __attribute__((always_inline)) void bandBgN(
    Snes* s, const BgLayer* bg, const int bpp, const uint16_t* pal, uint32_t prioBit, int y0, int n) {
  Ppu* ppu = &s->ppu;
  const uint32_t hScroll = bg->hScroll;
  const int xFine = (int)(hScroll & 7);
  const int cols = xFine ? 33 : 32;
  const int palShift = bpp == 2 ? 2 : 4;
  int line = 0;
  while(line < n) {
    const uint32_t ly = (uint32_t)(y0 + line + bg->vScroll) & 0x3ff;
    const int rowIn = (int)(ly & 7);
    int seg = 8 - rowIn;
    if(seg > n - line) seg = n - line;
    uint32_t rowAdr = bg->tilemapAdr + (((ly >> 3) & 0x1f) << 5);
    if((ly & 0x100) && bg->tilemapHigher) rowAdr += bg->tilemapWider ? 0x800 : 0x400;
    /* the 32 entries of a tilemap row are 64 bytes that never straddle two pages */
    const uint8_t* mapL = tileData(s, rowAdr);
    const uint8_t* mapR = bg->tilemapWider ? tileData(s, rowAdr + 0x400) : mapL;
    uint16_t* band = ppu->band + line * NS_SCREEN_W;
    uint32_t tcol = hScroll >> 3;
    int x = -xFine;
    for(int c = 0; c < cols; c++, tcol++, x += 8) {
      const uint8_t* e = ((tcol & 0x20) ? mapR : mapL) + ((tcol & 0x1f) << 1);
      const uint32_t tile = (uint32_t)(e[0] | (e[1] << 8));
      if((tile & 0x2000) != prioBit) continue;
      const uint8_t* data = tileData(s, bg->tileAdr + (tile & 0x3ff) * (uint32_t)(bpp * 4));
      /* a tile with nothing in it (most of a status bar layer, the sky...) */
      uint32_t any = load32(data) | load32(data + 4) | load32(data + 8) | load32(data + 12);
      if(bpp == 4) any |= load32(data + 16) | load32(data + 20) | load32(data + 24) | load32(data + 28);
      if(!any) continue;
      const uint32_t* lut = ppu->spread[(tile >> 14) & 1];
      const uint16_t* tp = pal + (((tile >> 10) & 7) << palShift);
      int trow = rowIn;
      int step = 1;
      if(tile & 0x8000) {
        trow = 7 - rowIn;
        step = -1;
      }
      const bool inside = x >= 0 && x <= NS_SCREEN_W - 8;
      uint16_t* row = band;
      for(int j = 0; j < seg; j++, trow += step, row += NS_SCREEN_W) {
        const uint8_t* p = data + trow * 2;
        uint32_t w = lut[p[0]] | (lut[p[1]] << 1);
        if(bpp == 4) w |= (lut[p[16]] << 2) | (lut[p[17]] << 3);
        if(!w) continue;
        if(inside) paint8(row + x, w, tp);
        else paintClipped(row, x, w, tp);
      }
    }
    line += seg;
  }
}

static __attribute__((noinline)) void bandBg2(Snes* s, const BgLayer* bg, const uint16_t* pal, uint32_t prioBit, int y0, int n) {
  bandBgN(s, bg, 2, pal, prioBit, y0, n);
}

static __attribute__((noinline)) void bandBg4(Snes* s, const BgLayer* bg, const uint16_t* pal, uint32_t prioBit, int y0, int n) {
  bandBgN(s, bg, 4, pal, prioBit, y0, n);
}

/* sprite pixels of one priority */
static void bandObj(Snes* s, const Plan* plan, uint32_t prio, int blend, int y0, int n) {
  Ppu* ppu = &s->ppu;
  const uint32_t want = (OBJ_PIXEL >> 8) | prio;
  const uint16_t* plain = ppu->pal565;
  const uint16_t* mixed = (blend == BLEND_PAL || blend == BLEND_ALL) ? ppu->palBlend : plain;
  for(int r = 0; r < n; r++) {
    if(!(ppu->objPrioMask[r] & (1u << prio))) continue;
    const uint16_t* buf = ppu->objRow[r];
    uint16_t* d = ppu->band + r * NS_SCREEN_W;
    for(int t = 0; t < ppu->objSlivers[r]; t++) {
      int x = ppu->objSliverX[r][t];
      int end = x + 8;
      if(x < 0) x = 0;
      if(end > NS_SCREEN_W) end = NS_SCREEN_W;
      for(; x < end; x++) {
        const uint32_t o = buf[x];
        if((o >> 8) != want) continue;
        const uint32_t idx = o & 0xff;
        /* palettes 0-3 never take part in colour math on the main screen */
        if(idx < 0xc0 && blend != BLEND_ALL) {
          d[x] = plain[idx];
        } else if(blend != BLEND_SUB) {
          d[x] = mixed[idx];
        } else {
          const int sub = subPixel(s, plan, x, y0 + r, r);
          d[x] = sub < 0 ? blend565(ppu, ppu->cgram[idx], fixedColor(ppu), false)
                         : blend565(ppu, ppu->cgram[idx], ppu->cgram[sub], ppu->halfColor);
        }
      }
    }
  }
}

static void bandFast(Snes* s, const Plan* plan, int y0, int n) {
  Ppu* ppu = &s->ppu;
  /* backdrop */
  {
    const uint16_t c = plan->backdrop;
    uint16_t* d = ppu->band;
    for(int i = 0; i < n * NS_SCREEN_W; i++) d[i] = c;
  }
  for(int i = 0; i < plan->n; i++) {
    const Pass* p = &plan->pass[i];
    if(p->layer == 4) {
      bandObj(s, plan, p->prio, p->blend, y0, n);
      continue;
    }
    const uint16_t* pal = (p->blend ? ppu->palBlend : ppu->pal565) + (ppu->mode == 0 ? 32 * p->layer : 0);
    const BgLayer* bg = &ppu->bgLayer[p->layer];
    if(bitDepthsPerMode[plan->mode][p->layer] == 2) bandBg2(s, bg, pal, p->prio ? 0x2000 : 0, y0, n);
    else bandBg4(s, bg, pal, p->prio ? 0x2000 : 0, y0, n);
  }
}

/* --------------------------------------------------------------- entry -- */

void ppu_init(Snes* s) {
  Ppu* ppu = &s->ppu;
  for(uint32_t b = 0; b < 0x100; b++) {
    uint32_t normal = 0, flipped = 0;
    for(int i = 0; i < 8; i++) {
      if(b & (0x80u >> i)) normal |= 1u << (4 * i);
      if(b & (1u << i)) flipped |= 1u << (4 * i);
    }
    ppu->spread[0][b] = normal;
    ppu->spread[1][b] = flipped;
  }
}

/* draws lines first..last (1-based), all with the current register state */
static void renderLines(Snes* s, int first, int last) {
  Ppu* ppu = &s->ppu;
  Plan plan;
  plan.n = 0;
  plan.mode = 0;
  plan.subShown = 0;
  bool fast = false;
  if(!ppu->forcedBlank) {
    ppu_buildLuts(ppu);
    fast = planBuild(s, &plan);
  }
  for(int y0 = first; y0 <= last;) {
    int n = last - y0 + 1;
    if(n > NS_BAND) n = NS_BAND;
    if(ppu->forcedBlank) {
      memset(ppu->band, 0, (size_t)n * NS_SCREEN_W * sizeof(uint16_t));
    } else {
      for(int r = 0; r < n; r++) evalSprites(s, y0 + r - 1, r);
      if(fast) {
        bandFast(s, &plan, y0, n);
        ppu->statFastLines += (uint32_t)n;
      } else {
        for(int r = 0; r < n; r++) slowLine(s, y0 + r, r, ppu->band + r * NS_SCREEN_W);
        ppu->statSlowLines += (uint32_t)n;
        ppu->statWhy[ppu->why & 7] += (uint32_t)n;
      }
    }
    s->putRows(s->user, y0 - 1, n, ppu->band);
    y0 += n;
  }
}

/* A line is "due" once the beam is halfway through it (LakeSnes draws it at
 * that point). Called before anything the picture depends on changes, and
 * when vertical blanking starts. */
void ppu_flush(Snes* s) {
  Ppu* ppu = &s->ppu;
  /* the register writes that were waiting, each after the lines before it */
  if(ppu->wlogCount) {
    const unsigned n = ppu->wlogCount;
    ppu->wlogCount = 0;
    for(unsigned i = 0; i < n; i++) {
      const uint32_t e = ppu->wlog[i];
      const int line = (int)(e >> 16);
      if(line > ppu->renderedLine) {
        const int first = ppu->renderedLine + 1;
        ppu->renderedLine = (uint8_t)line;
        renderLines(s, first, line);
      }
      ppu_apply(s, (uint8_t)(e >> 8), (uint8_t)e);
    }
  }
  if(s->inVblank || !s->renderFrame || s->refPpu || s->vPos == 0) return;
  int due = snes_hPos(s) >= 512 ? s->vPos : s->vPos - 1;
  if(due > NS_SCREEN_H_OVERSCAN) due = NS_SCREEN_H_OVERSCAN;
  if(due <= ppu->renderedLine) return;
  const int first = ppu->renderedLine + 1;
  ppu->renderedLine = (uint8_t)due;
  renderLines(s, first, due);
}
