/*
 * nwsnes - reference renderer (PC builds only)
 *
 * Derived from LakeSnes (https://github.com/angelo-wf/LakeSnes)
 * Copyright (c) 2021-2023 angelo_wf and contributors - MIT license.
 * This is LakeSnes' accurate, slow, one-pixel-at-a-time renderer. nwsnes keeps
 * it to check its own fast line renderer against (see host/).
 */
#ifdef NS_WITH_REFERENCE
#include <string.h>

#include "snes.h"

static Snes* R; /* instance being rendered */
static uint8_t refObjPixel[256];
static uint8_t refObjPrio[256];

// array for layer definitions per mode:
//   0-7: mode 0-7; 8: mode 1 + l3prio; 9: mode 7 + extbg

//   0-3; layers 1-4; 4: sprites; 5: nonexistent
static const int layersPerMode[10][12] = {
  {4, 0, 1, 4, 0, 1, 4, 2, 3, 4, 2, 3},
  {4, 0, 1, 4, 0, 1, 4, 2, 4, 2, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5},
  {4, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5, 5},
  {2, 4, 0, 1, 4, 0, 1, 4, 4, 2, 5, 5},
  {4, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5, 5}
};

static const int prioritysPerMode[10][12] = {
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

static const int layerCountPerMode[10] = {
  12, 10, 8, 8, 8, 8, 6, 5, 10, 7
};

static const int bitDepthsPerMode[10][4] = {
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

static const int spriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static void ppu_handlePixel(Ppu* ppu, int x, int y);
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
  s->putLine(s->user, line - 1, ppu->line);
}

static void ppu_handlePixel(Ppu* ppu, int x, int y) {
  int r = 0, r2 = 0;
  int g = 0, g2 = 0;
  int b = 0, b2 = 0;
  if(!ppu->forcedBlank) {
    int mainLayer = ppu_getPixel(ppu, x, y, false, &r, &g, &b);
    bool colorWindowState = ppu_getWindowState(ppu, 5, x);
    if(
      ppu->clipMode == 3 ||
      (ppu->clipMode == 2 && colorWindowState) ||
      (ppu->clipMode == 1 && !colorWindowState)
    ) {
      r = 0;
      g = 0;
      b = 0;
    }
    int secondLayer = 5; // backdrop
    bool mathEnabled = mainLayer < 6 && ppu->mathEnabled[mainLayer] && !(
      ppu->preventMathMode == 3 ||
      (ppu->preventMathMode == 2 && colorWindowState) ||
      (ppu->preventMathMode == 1 && !colorWindowState)
    );
    if((mathEnabled && ppu->addSubscreen) || ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6) {
      secondLayer = ppu_getPixel(ppu, x, y, true, &r2, &g2, &b2);
    }
    // TODO: subscreen pixels can be clipped to black as well
    // TODO: math for subscreen pixels (add/sub sub to main)
    if(mathEnabled) {
      if(ppu->subtractColor) {
        r -= (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g -= (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b -= (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      } else {
        r += (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g += (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b += (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      }
      if(ppu->halfColor && (secondLayer != 5 || !ppu->addSubscreen)) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
      }
      if(r > 31) r = 31;
      if(g > 31) g = 31;
      if(b > 31) b = 31;
      if(r < 0) r = 0;
      if(g < 0) g = 0;
      if(b < 0) b = 0;
    }
    if(!(ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6)) {
      r2 = r; g2 = g; b2 = b;
    }
  }
  // nwsnes: 256 pixels wide, RGB565, main screen pixel only
  ppu->line[x] = (uint16_t)((ppu->lut5[r] << 11) | (ppu->lut6[g] << 5) | ppu->lut5[b]);
  (void) r2; (void) g2; (void) b2;
}

static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b) {
  // figure out which color is on this location on main- or subscreen, sets it in r, g, b
  // returns which layer it is: 0-3 for bg layer, 4 or 6 for sprites (depending on palette), 5 for backdrop
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  actMode = ppu->mode == 7 && ppu->m7extBg ? 9 : actMode;
  int layer = 5;
  int pixel = 0;
  for(int i = 0; i < layerCountPerMode[actMode]; i++) {
    int curLayer = layersPerMode[actMode][i];
    int curPriority = prioritysPerMode[actMode][i];
    bool layerActive = false;
    if(!sub) {
      layerActive = ppu->layer[curLayer].mainScreenEnabled && (
        !ppu->layer[curLayer].mainScreenWindowed || !ppu_getWindowState(ppu, curLayer, x)
      );
    } else {
      layerActive = ppu->layer[curLayer].subScreenEnabled && (
        !ppu->layer[curLayer].subScreenWindowed || !ppu_getWindowState(ppu, curLayer, x)
      );
    }
    if(layerActive) {
      if(curLayer < 4) {
        // bg layer
        int lx = x;
        int ly = y;
        if(ppu->bgLayer[curLayer].mosaicEnabled && ppu->mosaicSize > 1) {
          lx -= lx % ppu->mosaicSize;
          ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
        }
        if(ppu->mode == 7) {
          pixel = ppu_getPixelForMode7(ppu, lx, curLayer, curPriority);
        } else {
          lx += ppu->bgLayer[curLayer].hScroll;
          if(ppu->mode == 5 || ppu->mode == 6) {
            lx *= 2;
            lx += (sub || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            if(ppu->interlace) {
              ly *= 2;
              ly += (ppu->evenFrame || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            }
          }
          ly += ppu->bgLayer[curLayer].vScroll;
          if(ppu->mode == 2 || ppu->mode == 4 || ppu->mode == 6) {
            ppu_handleOPT(ppu, curLayer, &lx, &ly);
          }
          pixel = ppu_getPixelForBgLayer(
            ppu, lx & 0x3ff, ly & 0x3ff,
            curLayer, curPriority
          );
        }
      } else {
        // get a pixel from the sprite buffer
        pixel = 0;
        if(refObjPrio[x] == curPriority) pixel = refObjPixel[x];
      }
    }
    if(pixel > 0) {
      layer = curLayer;
      break;
    }
  }
  if(ppu->directColor && layer < 4 && bitDepthsPerMode[actMode][layer] == 8) {
    *r = ((pixel & 0x7) << 2) | ((pixel & 0x100) >> 7);
    *g = ((pixel & 0x38) >> 1) | ((pixel & 0x200) >> 8);
    *b = ((pixel & 0xc0) >> 3) | ((pixel & 0x400) >> 8);
  } else {
    uint16_t color = ppu->cgram[pixel & 0xff];
    *r = color & 0x1f;
    *g = (color >> 5) & 0x1f;
    *b = (color >> 10) & 0x1f;
  }
  if(layer == 4 && pixel < 0xc0) layer = 6; // sprites with palette color < 0xc0
  return layer;
}

static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly) {
  int x = *lx;
  int y = *ly;
  int column = 0;
  if(ppu->mode == 6) {
    column = ((x - (x & 0xf)) - ((ppu->bgLayer[layer].hScroll * 2) & 0xfff0)) >> 4;
  } else {
    column = ((x - (x & 0x7)) - (ppu->bgLayer[layer].hScroll & 0xfff8)) >> 3;
  }
  if(column > 0) {
    // fetch offset values from layer 3 tilemap
    int valid = layer == 0 ? 0x2000 : 0x4000;
    uint16_t hOffset = ppu_getOffsetValue(ppu, column - 1, 0);
    uint16_t vOffset = 0;
    if(ppu->mode == 4) {
      if(hOffset & 0x8000) {
        vOffset = hOffset;
        hOffset = 0;
      }
    } else {
      vOffset = ppu_getOffsetValue(ppu, column - 1, 1);
    }
    if(ppu->mode == 6) {
      // TODO: not sure if correct
      if(hOffset & valid) *lx = (((hOffset & 0x3f8) + (column * 8)) * 2) | (x & 0xf);
    } else {
      if(hOffset & valid) *lx = ((hOffset & 0x3f8) + (column * 8)) | (x & 0x7);
    }
    // TODO: not sure if correct for interlace
    if(vOffset & valid) *ly = (vOffset & 0x3ff) + (y - ppu->bgLayer[layer].vScroll);
  }
}

static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row) {
  int x = col * 8 + ppu->bgLayer[2].hScroll;
  int y = row * 8 + ppu->bgLayer[2].vScroll;
  int tileBits = ppu->bgLayer[2].bigTiles ? 4 : 3;
  int tileHighBit = ppu->bgLayer[2].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[2].tilemapAdr + (((y >> tileBits) & 0x1f) << 5 | ((x >> tileBits) & 0x1f));
  if((x & tileHighBit) && ppu->bgLayer[2].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBit) && ppu->bgLayer[2].tilemapHigher) tilemapAdr += ppu->bgLayer[2].tilemapWider ? 0x800 : 0x400;
  return snes_vramRead(R, tilemapAdr & 0x7fff);
}

static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, bool priority) {
  // figure out address of tilemap word and read it
  bool wideTiles = ppu->bgLayer[layer].bigTiles || ppu->mode == 5 || ppu->mode == 6;
  int tileBitsX = wideTiles ? 4 : 3;
  int tileHighBitX = wideTiles ? 0x200 : 0x100;
  int tileBitsY = ppu->bgLayer[layer].bigTiles ? 4 : 3;
  int tileHighBitY = ppu->bgLayer[layer].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[layer].tilemapAdr + (((y >> tileBitsY) & 0x1f) << 5 | ((x >> tileBitsX) & 0x1f));
  if((x & tileHighBitX) && ppu->bgLayer[layer].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBitY) && ppu->bgLayer[layer].tilemapHigher) tilemapAdr += ppu->bgLayer[layer].tilemapWider ? 0x800 : 0x400;
  uint16_t tile = snes_vramRead(R, tilemapAdr & 0x7fff);
  // check priority, get palette
  if(((bool) (tile & 0x2000)) != priority) return 0; // wrong priority
  int paletteNum = (tile & 0x1c00) >> 10;
  // figure out position within tile
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int col = (tile & 0x4000) ? (x & 0x7) : 7 - (x & 0x7);
  int tileNum = tile & 0x3ff;
  if(wideTiles) {
    // if unflipped right half of tile, or flipped left half of tile
    if(((bool) (x & 8)) ^ ((bool) (tile & 0x4000))) tileNum += 1;
  }
  if(ppu->bgLayer[layer].bigTiles) {
    // if unflipped bottom half of tile, or flipped upper half of tile
    if(((bool) (y & 8)) ^ ((bool) (tile & 0x8000))) tileNum += 0x10;
  }
  // read tiledata, ajust palette for mode 0
  int bitDepth = bitDepthsPerMode[ppu->mode][layer];
  if(ppu->mode == 0) paletteNum += 8 * layer;
  // plane 1 (always)
  int paletteSize = 4;
  uint16_t plane1 = snes_vramRead(R, (ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + row) & 0x7fff);
  int pixel = (plane1 >> col) & 1;
  pixel |= ((plane1 >> (8 + col)) & 1) << 1;
  // plane 2 (for 4bpp, 8bpp)
  if(bitDepth > 2) {
    paletteSize = 16;
    uint16_t plane2 = snes_vramRead(R, (ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 8 + row) & 0x7fff);
    pixel |= ((plane2 >> col) & 1) << 2;
    pixel |= ((plane2 >> (8 + col)) & 1) << 3;
  }
  // plane 3 & 4 (for 8bpp)
  if(bitDepth > 4) {
    paletteSize = 256;
    uint16_t plane3 = snes_vramRead(R, (ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 16 + row) & 0x7fff);
    pixel |= ((plane3 >> col) & 1) << 4;
    pixel |= ((plane3 >> (8 + col)) & 1) << 5;
    uint16_t plane4 = snes_vramRead(R, (ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 24 + row) & 0x7fff);
    pixel |= ((plane4 >> col) & 1) << 6;
    pixel |= ((plane4 >> (8 + col)) & 1) << 7;
  }
  // return cgram index, or 0 if transparent, palette number in bits 10-8 for 8-color layers
  return pixel == 0 ? 0 : paletteSize * paletteNum + pixel;
}

static void ppu_calculateMode7Starts(Ppu* ppu, int y) {
  // expand 13-bit values to signed values
  int hScroll = ((int16_t) (ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t) (ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t) (ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t) (ppu->m7matrix[5] << 3)) >> 3;
  // do calculation
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  if(ppu->bgLayer[0].mosaicEnabled && ppu->mosaicSize > 1) {
    y -= (y - ppu->mosaicStartLine) % ppu->mosaicSize;
  }
  uint8_t ry = ppu->m7yFlip ? 255 - y : y;
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

static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority) {
  uint8_t rx = ppu->m7xFlip ? 255 - x : x;
  int xPos = (ppu->m7startX + ppu->m7matrix[0] * rx) >> 8;
  int yPos = (ppu->m7startY + ppu->m7matrix[2] * rx) >> 8;
  bool outsideMap = xPos < 0 || xPos >= 1024 || yPos < 0 || yPos >= 1024;
  xPos &= 0x3ff;
  yPos &= 0x3ff;
  if(!ppu->m7largeField) outsideMap = false;
  uint8_t tile = outsideMap ? 0 : snes_vramRead(R, (yPos >> 3) * 128 + (xPos >> 3)) & 0xff;
  uint8_t pixel = outsideMap && !ppu->m7charFill ? 0 : snes_vramRead(R, tile * 64 + (yPos & 7) * 8 + (xPos & 7)) >> 8;
  if(layer == 1) {
    if(((bool) (pixel & 0x80)) != priority) return 0;
    return pixel & 0x7f;
  }
  return pixel;
}

static bool ppu_getWindowState(Ppu* ppu, int layer, int x) {
  if(!ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    return false;
  }
  if(ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window1left && x <= ppu->window1right;
    return ppu->windowLayer[layer].window1inversed ? !test : test;
  }
  if(!ppu->windowLayer[layer].window1enabled && ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window2left && x <= ppu->window2right;
    return ppu->windowLayer[layer].window2inversed ? !test : test;
  }
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if(ppu->windowLayer[layer].window1inversed) test1 = !test1;
  if(ppu->windowLayer[layer].window2inversed) test2 = !test2;
  switch(ppu->windowLayer[layer].maskLogic) {
    case 0: return test1 || test2;
    case 1: return test1 && test2;
    case 2: return test1 != test2;
    case 3: return test1 == test2;
  }
  return false;
}

static void ppu_evaluateSprites(Ppu* ppu, int line) {
  // TODO: rectangular sprites, wierdness with sprites at -256
  uint8_t index = ppu->objPriority ? (ppu->oamAdr & 0xfe) : 0;
  int spritesFound = 0;
  int tilesFound = 0;
  uint8_t foundSprites[32] = {};
  // iterate over oam to find sprites in range
  for(int i = 0; i < 128; i++) {
    uint8_t y = ppu->oam[index] >> 8;
    // check if the sprite is on this line and get the sprite size
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    if(row < spriteHeight) {
      // in y-range, get the x location, using the high bit as well
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      if(x > 255) x -= 512;
      // if in x-range, record
      if(x > -spriteSize) {
        // break if we found 32 sprites already
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
  // iterate over found sprites backwards to fetch max 34 tile slivers
  for(int i = spritesFound; i > 0; i--) {
    index = foundSprites[i - 1];
    uint8_t y = ppu->oam[index] >> 8;
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x > 255) x -= 512;
    if(x > -spriteSize) {
      // update row according to obj-interlace
      if(ppu->objInterlace) row = row * 2 + (ppu->evenFrame ? 0 : 1);
      // get some data for the sprite and y-flip row if needed
      int tile = ppu->oam[index + 1] & 0xff;
      int palette = (ppu->oam[index + 1] & 0xe00) >> 9;
      bool hFlipped = ppu->oam[index + 1] & 0x4000;
      if(ppu->oam[index + 1] & 0x8000) row = spriteSize - 1 - row;
      // fetch all tiles in x-range
      for(int col = 0; col < spriteSize; col += 8) {
        if(col + x > -8 && col + x < 256) {
          // break if we found > 34 8*1 slivers already
          tilesFound++;
          if(tilesFound > 34) {
            ppu->timeOver = true;
            break;
          }
          // figure out which tile this uses, looping within 16x16 pages, and get it's data
          int usedCol = hFlipped ? spriteSize - 1 - col : col;
          uint8_t usedTile = (((tile >> 4) + (row / 8)) << 4) | (((tile & 0xf) + (usedCol / 8)) & 0xf);
          uint16_t objAdr = (ppu->oam[index + 1] & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
          uint16_t plane1 = snes_vramRead(R, (objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff);
          uint16_t plane2 = snes_vramRead(R, (objAdr + usedTile * 16 + 8 + (row & 0x7)) & 0x7fff);
          // go over each pixel
          for(int px = 0; px < 8; px++) {
            int shift = hFlipped ? px : 7 - px;
            int pixel = (plane1 >> shift) & 1;
            pixel |= ((plane1 >> (8 + shift)) & 1) << 1;
            pixel |= ((plane2 >> shift) & 1) << 2;
            pixel |= ((plane2 >> (8 + shift)) & 1) << 3;
            // draw it in the buffer if there is a pixel here
            int screenCol = col + x + px;
            if(pixel > 0 && screenCol >= 0 && screenCol < 256) {
              refObjPixel[screenCol] = 0x80 + 16 * palette + pixel;
              refObjPrio[screenCol] = (ppu->oam[index + 1] & 0x3000) >> 12;
            }
          }
        }
      }
      if(tilesFound > 34) break; // break out of sprite-loop if max tiles found
    }
  }
}

#endif /* NS_WITH_REFERENCE */
