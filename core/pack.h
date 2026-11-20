/*
 * nwsnes - game pack
 *
 * A pack is the single file installed next to the app on the calculator (as
 * the app's "external data"). It holds the ROM image, the page dictionary
 * built by profiling the game on a PC, and a few per-game settings.
 * Everything is little endian; nothing is assumed about alignment.
 *
 *   0  char[8] "NWSNESPK"
 *   8  u32 version (1)
 *  12  u32 header size (64)
 *  16  u32 total size
 *  20  u32 rom offset
 *  24  u32 rom size (power of two, mirrored if needed)
 *  28  u32 battery ram size (power of two or 0)
 *  32  u32 dictionary offset (entries of 8 bytes, sorted by hash)
 *  36  u32 dictionary entry count
 *  40  u8  cart type (1 LoROM, 2 HiROM, 3 ExHiROM)
 *  41  u8  1 if PAL
 *  42  u8  audio stand-in mode (0xff: automatic)
 *  43  u8  frame skip (0xff: automatic)
 *  44  u32 flags (0)
 *  48  char[16] title, zero padded
 */
#ifndef NWSNES_PACK_H
#define NWSNES_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snes.h"

#define NS_PACK_MAGIC "NWSNESPK"
#define NS_PACK_VERSION 1u
#define NS_PACK_HEADER_SIZE 64u

typedef struct NsPack {
  const uint8_t* base;
  uint32_t size;
  const uint8_t* rom;
  NsRomInfo info;
  const uint8_t* dict;
  uint32_t dictCount;
  uint8_t apuMode;
  uint8_t frameSkip;
  char title[17];
} NsPack;

/* Validates the pack and fills out. Returns false if it is not usable. */
bool pack_open(const uint8_t* data, size_t size, NsPack* out);
/* Plugs an opened pack into a core (rom, dictionary, settings). */
void pack_attach(const NsPack* pack, Snes* s);

#endif
