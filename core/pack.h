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
 *  44  u32 flags: bit 0 = the ROM is not complete (version 2 only)
 *  48  char[16] title, zero padded
 *
 * Version 2 (header of 80 bytes) is used when the whole ROM does not fit in
 * the calculator. The ROM is then stored as 4 KB blocks, and only the blocks
 * the game was seen to read while it was played on the PC (plus their
 * neighbours, as far as room allows) are in the pack:
 *
 *  64  u32 block table offset: one u32 per 4 KB block of the ROM, the offset
 *      of the block in the pack, or 0xffffffff when it is not there
 *  68  u32 number of blocks (rom size / 4096)
 *  72  u32 number of blocks present
 *  76  u32 size of the extension, or 0
 *
 * In version 2 the "rom offset" field is where the stored blocks start.
 *
 * The extension. The calculator's flash has two halves, each with an area
 * for apps, and apps are installed in the area of the half the system runs
 * from. A game too big for that area can have part of its ROM blocks written
 * to the app area of the other half, which an app can read but which the
 * system does not manage: that part is the extension. The header is then 96
 * bytes long:
 *
 *  80  u32 identifier of the extension (the same number is in its header)
 *  84  u32 ns_hashBytes() of the extension, header excluded
 *  88  8 bytes reserved (0)
 *
 * and a block table entry with bit 31 set is an offset inside the extension
 * (header included) instead of inside the pack. So is a dictionary entry
 * with bit 31 set: a page content that some ROM block of the extension
 * happens to hold. The extension starts with:
 *
 *   0  char[8] "NWSNESX1"
 *   8  u32 size of the extension, this header included
 *  12  u32 identifier
 *  16  16 bytes reserved (0)
 *
 * Blocks with the same content are stored once: several entries of the
 * block table may give the same place.
 *
 * In a .pack file on a PC the extension simply follows the pack. On the
 * calculator it sits at the end of the app area of the other half; the front
 * end finds it and hands it over with pack_setExtension().
 */
#ifndef NWSNES_PACK_H
#define NWSNES_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snes.h"

#define NS_PACK_MAGIC "NWSNESPK"
#define NS_PACK_VERSION 1u
#define NS_PACK_VERSION_BLOCKS 2u
#define NS_PACK_HEADER_SIZE 64u
#define NS_PACK_HEADER_SIZE_BLOCKS 80u
#define NS_PACK_HEADER_SIZE_EXT 96u
#define NS_PACK_FLAG_PARTIAL 1u
#define NS_PACK_EXT_MAGIC "NWSNESX1"
#define NS_PACK_EXT_HEADER_SIZE 32u
#define NS_PACK_IN_EXT 0x80000000u /* block table or dictionary entry: the place is in the extension */

typedef struct NsPack {
  const uint8_t* base;
  uint32_t size;
  const uint8_t* rom;        /* the ROM in one piece, or NULL... */
  const uint8_t* blockTable; /* ...when it comes as blocks (version 2) */
  uint32_t blocksPresent;    /* blocks of the ROM that are in the pack */
  uint32_t extSize;          /* size of the extension the pack goes with, or 0 */
  uint32_t extId;
  uint32_t extHash;
  const uint8_t* ext;        /* the extension, once found and checked; NULL otherwise */
  NsRomInfo info;
  const uint8_t* dict;
  uint32_t dictCount;
  uint8_t apuMode;
  uint8_t frameSkip;
  char title[17];
} NsPack;

/* Validates the pack and fills out. Returns false if it is not usable. */
bool pack_open(const uint8_t* data, size_t size, NsPack* out);
/* Gives the pack its extension (see above). Checks that it is the right one,
 * all of it when thorough is set (reads every byte), and returns false if not. */
bool pack_setExtension(NsPack* pack, const uint8_t* ext, bool thorough);
/* Plugs an opened pack into a core (rom, dictionary, settings). Returns false
 * when the core's pool is too small to hold the table of ROM blocks, or when
 * the pack needs an extension it has not been given. */
bool pack_attach(const NsPack* pack, Snes* s);
/* Where the extension lives on the calculator.
 *
 * The 8 MB of flash are two halves of 4 MB, each able to hold a whole
 * system: one runs, the other is the spare that updates are written to. Each
 * half describes itself 64 KB after its start, in NS_FLASH_INFO_SIZE bytes
 * (the same magic number first and last, the bounds of its area for apps at
 * offsets 20 and 24). The extension of a pack goes at the very end of the
 * app area of the half that is NOT running, the last sector of the half
 * excluded (the system keeps settings there).
 *
 * pack_extensionAddress() gives that place, or 0 when there is none:
 * address is any address inside the running half (the app's own, say),
 * thisInfo and otherInfo are the descriptions of the running half and of the
 * other one. The PC tool that writes the extension and the app that reads it
 * both go through this function, so they cannot disagree. */
#define NS_FLASH_ORIGIN 0x90000000u
#define NS_FLASH_HALF 0x00400000u
#define NS_FLASH_SECTOR 0x00010000u
#define NS_FLASH_INFO_OFFSET 0x00010000u
#define NS_FLASH_INFO_SIZE 48u
#define NS_FLASH_INFO_MAGIC 0xdec0edfeu
uint32_t pack_flashHalf(uint32_t address);      /* start of the half address is in; 0 if not in the flash */
uint32_t pack_otherFlashHalf(uint32_t address); /* start of the other half; 0 if not in the flash */
uint32_t pack_extensionAddress(uint32_t address, const uint8_t* thisInfo, const uint8_t* otherInfo, uint32_t extSize);

/* The 256 bytes a dictionary entry of the pack points to, given the entry's
 * offset field; NULL when they are in an extension the pack does not have. */
const uint8_t* pack_dictPage(const NsPack* pack, uint32_t offset);
/* hash of a run of bytes whose length is a multiple of 4 */
uint32_t ns_hashBytes(const uint8_t* data, uint32_t length);

#endif
