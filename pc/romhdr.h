/* nwsnes PC side - ROM header detection (not built for the calculator) */
#ifndef NWSNES_ROMHDR_H
#define NWSNES_ROMHDR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NsRomHeader {
  bool valid;         /* a plausible header was found */
  int copierHeader;   /* 512 if the file starts with a copier header, else 0 */
  uint8_t cartType;   /* 1 LoROM, 2 HiROM, 3 ExHiROM */
  bool pal;
  char title[22];
  uint8_t mapMode;    /* $FFD5 */
  uint8_t chipset;    /* $FFD6 */
  uint8_t coprocessor;/* 0 none, else 1 + high nibble of $FFD6 */
  uint32_t headerRomSize;
  uint32_t ramSize;   /* battery/cart RAM in bytes, 0 if none */
  uint8_t region;
  uint16_t checksum;
  uint16_t checksumComplement;
  int score;
} NsRomHeader;

/* data/length: the ROM file as found on disk */
void ns_readRomHeader(const uint8_t* data, int length, NsRomHeader* out);
/* name of the extra chip, or NULL when the cartridge has none */
const char* ns_coprocessorName(const NsRomHeader* h);

#endif
