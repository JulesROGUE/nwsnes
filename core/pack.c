/* nwsnes - game pack reader (see pack.h) */
#include "pack.h"

#include <string.h>

uint32_t ns_hashBytes(const uint8_t* data, uint32_t length) {
  uint32_t h = 0x9e3779b9u;
  for(uint32_t i = 0; i + 4 <= length; i += 4) {
    h ^= ns_rd32(data + i);
    h *= 0x85ebca6bu;
    h = (h << 13) | (h >> 19);
  }
  h ^= h >> 16;
  h *= 0xc2b2ae35u;
  h ^= h >> 13;
  return h;
}

bool pack_setExtension(NsPack* pack, const uint8_t* ext, bool thorough) {
  pack->ext = NULL;
  if(!pack->extSize || !ext) return false;
  if(memcmp(ext, NS_PACK_EXT_MAGIC, 8) != 0) return false;
  if(ns_rd32(ext + 8) != pack->extSize || ns_rd32(ext + 12) != pack->extId) return false;
  if(thorough && ns_hashBytes(ext + NS_PACK_EXT_HEADER_SIZE, pack->extSize - NS_PACK_EXT_HEADER_SIZE) != pack->extHash) return false;
  pack->ext = ext;
  return true;
}

bool pack_open(const uint8_t* data, size_t size, NsPack* out) {
  memset(out, 0, sizeof(*out));
  if(data == NULL || size < NS_PACK_HEADER_SIZE) return false;
  if(memcmp(data, NS_PACK_MAGIC, 8) != 0) return false;
  const uint32_t version = ns_rd32(data + 8);
  if(version != NS_PACK_VERSION && version != NS_PACK_VERSION_BLOCKS) return false;
  uint32_t headerSize = ns_rd32(data + 12);
  uint32_t total = ns_rd32(data + 16);
  uint32_t romOff = ns_rd32(data + 20);
  uint32_t romSize = ns_rd32(data + 24);
  uint32_t ramSize = ns_rd32(data + 28);
  uint32_t dictOff = ns_rd32(data + 32);
  uint32_t dictCount = ns_rd32(data + 36);
  if(headerSize < NS_PACK_HEADER_SIZE || total > size || total < headerSize) return false;
  if(romSize < 0x8000 || (romSize & (romSize - 1)) != 0) return false;
  const uint8_t* blockTable = NULL;
  uint32_t blocksPresent = romSize >> NS_ROM_BLOCK_SHIFT;
  uint32_t extSize = 0;
  if(version == NS_PACK_VERSION) {
    if(romOff > total || romSize > total - romOff) return false;
  } else {
    /* the ROM as a table of 4 KB blocks, some of which may be missing */
    if(headerSize < NS_PACK_HEADER_SIZE_BLOCKS || size < NS_PACK_HEADER_SIZE_BLOCKS) return false;
    const uint32_t tableOff = ns_rd32(data + 64);
    const uint32_t blocks = ns_rd32(data + 68);
    if(blocks != romSize >> NS_ROM_BLOCK_SHIFT) return false;
    if(tableOff > total || blocks * 4 > total - tableOff) return false;
    extSize = ns_rd32(data + 76);
    if(extSize && (headerSize < NS_PACK_HEADER_SIZE_EXT || extSize < NS_PACK_EXT_HEADER_SIZE || extSize >= NS_PACK_IN_EXT)) return false;
    blockTable = data + tableOff;
    blocksPresent = 0;
    for(uint32_t i = 0; i < blocks; i++) {
      const uint32_t off = ns_rd32(blockTable + i * 4);
      if(off == 0xffffffffu) continue;
      if(off & NS_PACK_IN_EXT) {
        const uint32_t e = off & ~NS_PACK_IN_EXT;
        if(e < NS_PACK_EXT_HEADER_SIZE || e > extSize || extSize - e < NS_ROM_BLOCK) return false;
      } else if(off > total || total - off < NS_ROM_BLOCK) {
        return false;
      }
      blocksPresent++;
    }
  }
  if(ramSize & (ramSize - 1)) return false;
  if(dictCount > 0x100000) return false;
  if(dictOff > total || dictCount * NS_DICT_ENTRY_SIZE > total - dictOff) return false;
  uint8_t cartType = data[40];
  if(cartType < NS_CART_LOROM || cartType > NS_CART_EXHIROM) return false;
  /* every dictionary page must lie inside the pack */
  const uint8_t* dict = data + dictOff;
  for(uint32_t i = 0; i < dictCount; i++) {
    uint32_t off = ns_rd32(dict + i * NS_DICT_ENTRY_SIZE + 4);
    if(off & NS_PACK_IN_EXT) {
      /* ...or inside the extension */
      const uint32_t e = off & ~NS_PACK_IN_EXT;
      if(e < NS_PACK_EXT_HEADER_SIZE || e > extSize || extSize - e < NS_PAGE_SIZE) return false;
    } else if(off > total || total - off < NS_PAGE_SIZE) {
      return false;
    }
  }
  out->base = data;
  out->size = total;
  out->rom = blockTable ? NULL : data + romOff;
  out->blockTable = blockTable;
  out->blocksPresent = blocksPresent;
  if(extSize) {
    out->extSize = extSize;
    out->extId = ns_rd32(data + 80);
    out->extHash = ns_rd32(data + 84);
    /* in a file, the extension follows the pack (on the calculator it does
     * not: size is the pack's own there, and the front end goes and finds it) */
    if(size >= (size_t)total + extSize) pack_setExtension(out, data + total, true);
  }
  out->info.cartType = cartType;
  out->info.pal = data[41] != 0;
  out->info.romSize = romSize;
  out->info.ramSize = ramSize;
  out->dict = dict;
  out->dictCount = dictCount;
  out->apuMode = data[42];
  out->frameSkip = data[43];
  /* the title ends up on screen: keep it to plain printable characters */
  for(int i = 0; i < 16; i++) {
    uint8_t ch = data[48 + i];
    out->title[i] = ch == 0 ? 0 : ((ch >= 0x20 && ch < 0x7f) ? (char)ch : '?');
  }
  out->title[16] = 0;
  return true;
}

uint32_t pack_flashHalf(uint32_t address) {
  if(address < NS_FLASH_ORIGIN || address - NS_FLASH_ORIGIN >= 2 * NS_FLASH_HALF) return 0;
  return NS_FLASH_ORIGIN + ((address - NS_FLASH_ORIGIN) & ~(NS_FLASH_HALF - 1));
}

uint32_t pack_otherFlashHalf(uint32_t address) {
  const uint32_t half = pack_flashHalf(address);
  if(!half) return 0;
  return half == NS_FLASH_ORIGIN ? NS_FLASH_ORIGIN + NS_FLASH_HALF : NS_FLASH_ORIGIN;
}

/* the app area a half declares; false when the description is not a sane one */
static bool appArea(uint32_t half, const uint8_t* info, uint32_t* start, uint32_t* end) {
  if(!info || ns_rd32(info) != NS_FLASH_INFO_MAGIC || ns_rd32(info + 44) != NS_FLASH_INFO_MAGIC) return false;
  *start = ns_rd32(info + 20);
  *end = ns_rd32(info + 24);
  return *start >= half + NS_FLASH_INFO_OFFSET && *end <= half + NS_FLASH_HALF && *start < *end &&
         (*end & (NS_FLASH_SECTOR - 1)) == 0;
}

uint32_t pack_extensionAddress(uint32_t address, const uint8_t* thisInfo, const uint8_t* otherInfo, uint32_t extSize) {
  const uint32_t half = pack_flashHalf(address), other = pack_otherFlashHalf(address);
  if(!half || !extSize) return 0;
  uint32_t start, end;
  if(!appArea(other, otherInfo, &start, &end)) {
    /* no system in the other half: the same place there as in this one */
    if(!appArea(half, thisInfo, &start, &end)) return 0;
    start = start - half + other;
    end = end - half + other;
  }
  if(end > other + NS_FLASH_HALF - NS_FLASH_SECTOR) end = other + NS_FLASH_HALF - NS_FLASH_SECTOR;
  start = (start + NS_FLASH_SECTOR - 1) & ~(NS_FLASH_SECTOR - 1);
  const uint32_t need = (extSize + NS_FLASH_SECTOR - 1) & ~(NS_FLASH_SECTOR - 1);
  if(need < extSize || start >= end || need > end - start) return 0;
  return end - need;
}

const uint8_t* pack_dictPage(const NsPack* pack, uint32_t offset) {
  if(!(offset & NS_PACK_IN_EXT)) return pack->base + offset;
  return pack->ext ? pack->ext + (offset & ~NS_PACK_IN_EXT) : NULL;
}

bool pack_attach(const NsPack* pack, Snes* s) {
  if(pack->extSize && !pack->ext) return false;
  const bool ok = pack->blockTable ? snes_setRomBlocks(s, pack->base, pack->ext, pack->blockTable, &pack->info)
                                   : snes_setRom(s, pack->rom, &pack->info);
  pager_setDictionary(&s->pager, pack->base, pack->ext, pack->dict, pack->dictCount);
  s->apu.forcedMode = pack->apuMode;
  return ok;
}
