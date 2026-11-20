/*
 * nwsnes PC side - memory profiler and pack builder
 *
 * Runs next to a core that has plenty of RAM, watches what every 256-byte
 * page of emulated memory contains from frame to frame, and keeps the
 * contents that stay put for a while. Those contents become the dictionary
 * stored in flash, which is what lets the calculator keep them out of RAM.
 */
#ifndef NWSNES_PROFILER_H
#define NWSNES_PROFILER_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/snes.h"

typedef struct NsProfiler NsProfiler;

typedef struct NsPackParams {
  const uint8_t* rom;    /* padded to a power of two */
  uint32_t romSize;
  uint8_t cartType;
  bool pal;
  uint32_t ramSize;
  uint8_t apuMode;       /* 0xff: automatic */
  uint8_t frameSkip;     /* 0xff: automatic */
  const char* title;
  uint32_t maxPackBytes; /* flash budget for the whole pack */
} NsPackParams;

typedef struct NsPackStats {
  uint32_t framesSampled;
  uint32_t candidates;     /* distinct stable page contents seen */
  uint32_t dropped;        /* contents not recorded because the table was full */
  uint32_t dictEntries;    /* entries written to the pack */
  uint32_t dictInRom;      /* of which point into the ROM image (free) */
  uint32_t dictStored;     /* of which are stored as 256 bytes of flash */
  uint32_t dictLeftOut;    /* candidates that did not fit in the budget */
  uint32_t packBytes;
} NsPackStats;

NsProfiler* prof_new(void);
void prof_free(NsProfiler* p);
/* Starts from the dictionary of an existing pack, so that a new play session
 * adds to what earlier ones found instead of replacing it. */
void prof_seed(NsProfiler* p, const uint8_t* packData, uint32_t packSize);
/* call once per emulated frame */
void prof_sample(NsProfiler* p, const Snes* s);
/* returns a malloc'ed pack (NULL on failure); stats may be NULL */
uint8_t* prof_buildPack(const NsProfiler* p, const NsPackParams* params, uint32_t* outSize, NsPackStats* stats);
/* pads/mirrors a ROM image to a power of two; returns a malloc'ed copy */
uint8_t* ns_padRom(const uint8_t* data, uint32_t length, uint32_t* outSize);

#endif
