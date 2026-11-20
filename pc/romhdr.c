/*
 * nwsnes PC side - ROM header detection
 *
 * The scoring comes from LakeSnes (c) 2021-2023 angelo_wf and contributors,
 * MIT license.
 */
#include "romhdr.h"

#include <string.h>

static void readHeader(const uint8_t* data, int length, int location, NsRomHeader* h) {
  memset(h, 0, sizeof(*h));
  for(int i = 0; i < 21; i++) {
    uint8_t ch = data[location + i];
    h->title[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
  }
  h->title[21] = 0;
  for(int i = 20; i >= 0 && h->title[i] == ' '; i--) h->title[i] = 0;
  uint8_t speed = data[location + 0x15] >> 4;
  uint8_t type = data[location + 0x15] & 0xf;
  h->mapMode = data[location + 0x15];
  h->chipset = data[location + 0x16];
  uint8_t coprocessor = data[location + 0x16] >> 4;
  uint8_t chips = data[location + 0x16] & 0xf;
  h->headerRomSize = 0x400u << (data[location + 0x17] & 0x1f);
  uint32_t ramSize = 0x400u << (data[location + 0x18] & 0x1f);
  h->region = data[location + 0x19];
  h->checksumComplement = (uint16_t)((data[location + 0x1d] << 8) + data[location + 0x1c]);
  h->checksum = (uint16_t)((data[location + 0x1f] << 8) + data[location + 0x1e]);
  h->pal = (h->region >= 0x2 && h->region <= 0xc) || h->region == 0x11;
  h->cartType = location < 0x9000 ? 1 : 2;
  if(location > 0x400000) h->cartType = 3;
  /* $FFD6: x0 rom, x1 rom+ram, x2 rom+ram+battery, x3 and up: extra chip */
  h->coprocessor = chips >= 3 ? (uint8_t)(1 + coprocessor) : 0;
  h->ramSize = (chips == 1 || chips == 2 || chips == 4 || chips == 5) && data[location + 0x18] != 0 ? ramSize : 0;
  int score = 0;
  score += (speed == 2 || speed == 3) ? 5 : -4;
  score += (type <= 3 || type == 5) ? 5 : -2;
  score += (coprocessor <= 5 || coprocessor >= 0xe) ? 5 : -2;
  score += (chips <= 6 || chips == 9 || chips == 0xa) ? 5 : -2;
  score += (h->region <= 0x14) ? 5 : -2;
  score += (h->checksum + h->checksumComplement == 0xffff) ? 8 : -6;
  uint16_t resetVector = (uint16_t)(data[location + 0x3c] | (data[location + 0x3d] << 8));
  score += (resetVector >= 0x8000) ? 8 : -20;
  int opcodeLoc = location + 0x40 - 0x8000 + (resetVector & 0x7fff);
  uint8_t opcode = 0xff;
  if(opcodeLoc < length) opcode = data[opcodeLoc];
  else score -= 14;
  if(opcode == 0x78 || opcode == 0x18) score += 6;                   /* sei, clc */
  if(opcode == 0x4c || opcode == 0x5c || opcode == 0x9c) score += 3; /* jmp, jml, stz */
  if(opcode == 0x00 || opcode == 0xff || opcode == 0xdb) score -= 6; /* brk, sbc alx, stp */
  h->score = score;
}

void ns_readRomHeader(const uint8_t* data, int length, NsRomHeader* out) {
  static const int locations[6] = {0x7fc0, 0x81c0, 0xffc0, 0x101c0, 0x40ffc0, 0x4101c0};
  NsRomHeader best;
  memset(&best, 0, sizeof(best));
  int bestScore = 0, bestIdx = -1;
  /* backwards, to prefer ExHiROM over HiROM when both look valid */
  for(int i = 5; i >= 0; i--) {
    if(length < locations[i] + 0x40) continue;
    NsRomHeader h;
    readHeader(data, length, locations[i], &h);
    if(h.score > bestScore) {
      bestScore = h.score;
      bestIdx = i;
      best = h;
    }
  }
  *out = best;
  out->valid = bestIdx >= 0;
  out->copierHeader = (bestIdx >= 0 && (bestIdx & 1)) ? 512 : 0;
}

const char* ns_coprocessorName(const NsRomHeader* h) {
  static const char* const names[16] = {
    "DSP", "Super FX (GSU)", "OBC1", "SA-1", "S-DD1", "S-RTC", "?", "?",
    "?", "?", "?", "?", "?", "?", "Super Game Boy / BS-X", "custom chip"
  };
  if(h->coprocessor == 0) return NULL;
  return names[(h->coprocessor - 1) & 0xf];
}
