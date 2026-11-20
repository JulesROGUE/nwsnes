/*
 * nwsnes - line renderer
 *
 * Draws one scanline at a time straight into a 256-pixel RGB565 buffer that
 * is handed to the display: there is no frame buffer anywhere.
 *
 * Every layer writes into a "z-buffer" line where each pixel holds
 *   bits 15-12  priority level (higher wins)
 *   bits 11-8   layer code: 0-3 backgrounds, 4 sprites taking part in colour
 *               math, 6 sprites that do not, 5 backdrop
 *   bits 7-0    palette index
 * so layers can be drawn in any order, then one pass turns the winners into
 * colours (with colour math against the sub screen when the game uses it).
 *
 * The priority tables, the pixel-exact slow paths (mosaic, offset-per-tile,
 * hi-res) and the general semantics come from LakeSnes (c) 2021-2023
 * angelo_wf and contributors, MIT license; the z-buffer idea follows the
 * snesrev renderer (c) 2021-2023 snesrev / elzo_d, MIT license. Output is
 * checked against LakeSnes' renderer (ppu_ref.c) by the host tests.
 *
 * Not handled by the fast renderer: direct colour, and 512-pixel modes are
 * shown at 256 pixels.
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
  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    for(int x = sp->edge[i]; x < sp->edge[i + 1]; x++) {
      int lx = x;
      int ly = y;
      if(mosaic) {
        lx -= lx % ppu->mosaicSize;
        ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
      }
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
      bool prio;
      int pixel = bgPixel(s, lx & 0x3ff, ly & 0x3ff, layer, bpp, &prio);
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

/* Fills objBuf for one line. Same selection rules as LakeSnes: at most 32
 * sprites and 34 tile slivers per line, later sprites are dropped first. */
/* lists the sprites that are on screen on at least one line */
static void buildObjList(Ppu* ppu) {
  int n = 0;
  for(int i = 0; i < 128; i++) {
    const int index = i * 2;
    const int y = ppu->oam[index] >> 8;
    const int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    const int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    /* rows wrap at 256: a sprite low enough comes back in at the top */
    if(y >= NS_SCREEN_H_OVERSCAN && y + spriteHeight <= 256) continue;
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x > 255) x -= 512;
    if(x <= -spriteSize) continue;
    ppu->objList[n++] = (uint8_t)index;
  }
  ppu->objCount = (uint8_t)n;
  ppu->objListDirty = false;
}

static bool evaluateSprites(Snes* s, int line, const Levels* lv) {
  Ppu* ppu = &s->ppu;
  /* with priority rotation the scan starts anywhere: take the long road */
  const bool rotate = ppu->objPriority;
  if(!rotate && ppu->objListDirty) buildObjList(ppu);
  const int scanCount = rotate ? 128 : ppu->objCount;
  uint8_t index = rotate ? (ppu->oamAdr & 0xfe) : 0;
  int spritesFound = 0;
  int tilesFound = 0;
  bool any = false;
  uint8_t foundSprites[32];
  for(int i = 0; i < scanCount; i++) {
    if(!rotate) index = ppu->objList[i];
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
    index += 2;
  }
  /* iterate over found sprites backwards to fetch max 34 tile slivers */
  for(int i = spritesFound; i > 0; i--) {
    index = foundSprites[i - 1];
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
    const uint32_t color = 0x80u + 16u * ((attr & 0xe00) >> 9);
    /* palettes 4-7 take part in colour math, 0-3 do not */
    const uint16_t z = (uint16_t)(lv->obj[(attr & 0x3000) >> 12] | ((color >= 0xc0 ? LAYER_OBJ : LAYER_OBJ_NOMATH) << 8));
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
          if(col + x + from < ppu->objMinX) ppu->objMinX = (uint16_t)(col + x + from);
          if(col + x + to > ppu->objMaxX) ppu->objMaxX = (uint16_t)(col + x + to);
          for(int k = from; k < to; k++) {
            if(px[k]) {
              ppu->objBuf[col + x + k] = (uint16_t)(z | (color + px[k]));
              any = true;
            }
          }
        }
      }
    }
    if(tilesFound > 34) break;
  }
  return any;
}

static void mergeSprites(const Ppu* ppu, uint16_t* dst, const Spans* sp) {
  for(int i = 0; i < sp->n; i++) {
    if(sp->masked & (1 << i)) continue;
    /* only the columns sprites were drawn in */
    int x = sp->edge[i] > ppu->objMinX ? sp->edge[i] : ppu->objMinX;
    const int end = sp->edge[i + 1] < ppu->objMaxX ? sp->edge[i + 1] : ppu->objMaxX;
    for(; x < end; x++) {
      uint16_t o = ppu->objBuf[x];
      if(o > dst[x]) dst[x] = o;
    }
  }
}

/* ------------------------------------------------------- composition -- */

static void drawScreen(Snes* s, int y, bool sub, const Levels* lv) {
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
  if(ppu->objBufDirty) {
    const Layer* l = &ppu->layer[4];
    if(sub ? l->subScreenEnabled : l->mainScreenEnabled) {
      getSpans(ppu, 4, sub ? l->subScreenWindowed : l->mainScreenWindowed, &sp);
      mergeSprites(ppu, dst, &sp);
    }
  }
}

void ppu_runLine(Snes* s, int line) {
  Ppu* ppu = &s->ppu;
#ifdef NS_WITH_REFERENCE
  if(s->refPpu) {
    ppu_refRunLine(s, line);
    return;
  }
#endif
  uint16_t* out = ppu->line;
  if(ppu->forcedBlank) {
    memset(out, 0, sizeof(ppu->line));
    s->putLine(s->user, line - 1, out);
    return;
  }
  ppu_buildLuts(ppu);
  Levels lv;
  getLevels(ppu, &lv);
  if(ppu->objBufDirty) {
    if(ppu->objMaxX > ppu->objMinX) {
      memset(ppu->objBuf + ppu->objMinX, 0, (size_t)(ppu->objMaxX - ppu->objMinX) * sizeof(uint16_t));
    }
    ppu->objBufDirty = false;
  }
  ppu->objMinX = NS_SCREEN_W;
  ppu->objMaxX = 0;
  if(evaluateSprites(s, line - 1, &lv)) ppu->objBufDirty = true;
  if(ppu->mode == 7) calculateMode7Starts(ppu, line);

  drawScreen(s, line, false, &lv);

  /* which layer codes take part in colour math (code 6, sprites with a
   * palette below 4, never does) */
  uint32_t mathMask = 0;
  for(int i = 0; i < 6; i++) mathMask |= (uint32_t)ppu->mathEnabled[i] << i;
  const bool useSub = mathMask && ppu->addSubscreen && ppu->preventMathMode != 3;
  if(useSub) drawScreen(s, line, true, &lv);

  const uint16_t* zm = ppu->zbuf[0];
  const uint16_t* zs = ppu->zbuf[1];
  const uint16_t* cgram = ppu->cgram;
  const uint16_t* pal565 = ppu->pal565;
  const uint8_t* lut5 = ppu->lut5;
  const uint8_t* lut6 = ppu->lut6;
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
      int r = 0, g = 0, b = 0;
      if(!clip) {
        const uint16_t c = cgram[z & 0xff];
        r = c & 0x1f;
        g = (c >> 5) & 0x1f;
        b = (c >> 10) & 0x1f;
      }
      int r2 = ppu->fixedColorR, g2 = ppu->fixedColorG, b2 = ppu->fixedColorB;
      bool half = ppu->halfColor;
      if(ppu->addSubscreen) {
        const uint16_t z2 = zs[x];
        if(((z2 >> 8) & 0xf) != LAYER_BACKDROP) {
          const uint16_t c2 = cgram[z2 & 0xff];
          r2 = c2 & 0x1f;
          g2 = (c2 >> 5) & 0x1f;
          b2 = (c2 >> 10) & 0x1f;
        } else {
          half = false; /* the fixed colour standing in for the sub screen is never halved */
        }
      }
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
      lastOut = (uint16_t)((lut5[r] << 11) | (lut6[g] << 5) | lut5[b]);
      out[x] = lastOut;
    }
  }
  s->putLine(s->user, line - 1, out);
}
