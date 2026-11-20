/* nwsnes - game pack reader (see pack.h) */
#include "pack.h"

#include <string.h>

bool pack_open(const uint8_t* data, size_t size, NsPack* out) {
  memset(out, 0, sizeof(*out));
  if(data == NULL || size < NS_PACK_HEADER_SIZE) return false;
  if(memcmp(data, NS_PACK_MAGIC, 8) != 0) return false;
  if(ns_rd32(data + 8) != NS_PACK_VERSION) return false;
  uint32_t headerSize = ns_rd32(data + 12);
  uint32_t total = ns_rd32(data + 16);
  uint32_t romOff = ns_rd32(data + 20);
  uint32_t romSize = ns_rd32(data + 24);
  uint32_t ramSize = ns_rd32(data + 28);
  uint32_t dictOff = ns_rd32(data + 32);
  uint32_t dictCount = ns_rd32(data + 36);
  if(headerSize < NS_PACK_HEADER_SIZE || total > size || total < headerSize) return false;
  if(romSize < 0x8000 || (romSize & (romSize - 1)) != 0) return false;
  if(romOff > total || romSize > total - romOff) return false;
  if(ramSize & (ramSize - 1)) return false;
  if(dictCount > 0x100000) return false;
  if(dictOff > total || dictCount * NS_DICT_ENTRY_SIZE > total - dictOff) return false;
  uint8_t cartType = data[40];
  if(cartType < NS_CART_LOROM || cartType > NS_CART_EXHIROM) return false;
  /* every dictionary page must lie inside the pack */
  const uint8_t* dict = data + dictOff;
  for(uint32_t i = 0; i < dictCount; i++) {
    uint32_t off = ns_rd32(dict + i * NS_DICT_ENTRY_SIZE + 4);
    if(off > total || total - off < NS_PAGE_SIZE) return false;
  }
  out->base = data;
  out->size = total;
  out->rom = data + romOff;
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

void pack_attach(const NsPack* pack, Snes* s) {
  snes_setRom(s, pack->rom, &pack->info);
  pager_setDictionary(&s->pager, pack->base, pack->dict, pack->dictCount);
  s->apu.forcedMode = pack->apuMode;
}
