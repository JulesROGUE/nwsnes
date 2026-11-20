/*
 * nwsnes as a WebAssembly module, for the snes2nw PC tool (Node.js and the
 * browser). It bundles the calculator core, the profiler and the pack
 * builder behind a small C API; every export is prefixed nsw_.
 *
 * One ROM is loaded at a time, and one emulation session exists at a time:
 * either a profiling session (plenty of RAM, pages are recorded) or a device
 * session (a pack and a RAM pool of a given size, as on the calculator).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../core/pack.h"
#include "../core/snes.h"
#include "../pc/autoinput.h"
#include "../pc/profiler.h"
#include "../pc/romhdr.h"

#define EXPORT(name) __attribute__((export_name(#name))) name

static NsRomHeader g_hdr;
static uint8_t* g_rom; /* padded image */
static uint32_t g_romSize;
static uint8_t* g_basePack; /* the ROM as a pack without dictionary */
static uint32_t g_basePackSize;
static NsProfiler* g_prof;
static uint8_t* g_built; /* last pack built */
static uint32_t g_builtSize;
static NsPackStats g_builtStats;

static Snes g_snes;
static NsPack g_pack;
static uint8_t* g_pool;
static uint16_t g_fb[NS_SCREEN_W * NS_SCREEN_H_OVERSCAN];
static const uint16_t* g_movie; /* recorded inputs, one per frame, or NULL */
static uint32_t g_movieLen;

static void putLine(void* user, int y, const uint16_t* line) {
  (void)user;
  if(y >= 0 && y < NS_SCREEN_H_OVERSCAN) memcpy(g_fb + y * NS_SCREEN_W, line, NS_SCREEN_W * sizeof(uint16_t));
}

static uint16_t inputAt(uint32_t seed, uint32_t frame) {
  if(g_movie) return frame < g_movieLen ? g_movie[frame] : 0;
  return ns_autoInput(seed, frame);
}

void* EXPORT(nsw_alloc)(uint32_t size) { return malloc(size ? size : 1); }
void EXPORT(nsw_free)(void* p) { free(p); }

static void fillParams(NsPackParams* params) {
  memset(params, 0, sizeof(*params));
  params->rom = g_rom;
  params->romSize = g_romSize;
  params->cartType = g_hdr.cartType;
  params->pal = g_hdr.pal;
  params->ramSize = g_hdr.ramSize > 0x8000 ? 0x8000 : g_hdr.ramSize;
  params->title = g_hdr.title;
  params->apuMode = 0xff;
  params->frameSkip = 0xff;
}

/* Loads a ROM file. Returns 0, or 1 when no SNES header is recognised. */
int EXPORT(nsw_load_rom)(const uint8_t* file, uint32_t size) {
  free(g_rom);
  free(g_basePack);
  g_rom = g_basePack = NULL;
  ns_readRomHeader(file, (int)size, &g_hdr);
  if(!g_hdr.valid) return 1;
  g_rom = ns_padRom(file + g_hdr.copierHeader, size - (uint32_t)g_hdr.copierHeader, &g_romSize);
  NsPackParams params;
  fillParams(&params);
  g_basePack = prof_buildPack(NULL, &params, &g_basePackSize, NULL);
  return g_basePack ? 0 : 2;
}

const char* EXPORT(nsw_title)(void) { return g_hdr.title; }
const char* EXPORT(nsw_coprocessor_name)(void) { return ns_coprocessorName(&g_hdr); }

/* 0 cart type, 1 PAL, 2 padded rom size, 3 battery ram size, 4 extra chip
 * (0 none), 5 copier header bytes, 6 header checksum valid, 7 map mode byte */
uint32_t EXPORT(nsw_info)(int id) {
  switch(id) {
    case 0: return g_hdr.cartType;
    case 1: return g_hdr.pal;
    case 2: return g_romSize;
    case 3: return g_hdr.ramSize;
    case 4: return g_hdr.coprocessor;
    case 5: return (uint32_t)g_hdr.copierHeader;
    case 6: return (uint16_t)(g_hdr.checksum + g_hdr.checksumComplement) == 0xffff;
    case 7: return g_hdr.mapMode;
    default: return 0;
  }
}

/* ---- sessions ---------------------------------------------------------- */

/* pack == NULL: the loaded ROM without dictionary. poolPages == 0: unlimited.
 * Returns 0 on success. The pack memory must stay allocated. */
int EXPORT(nsw_session)(const uint8_t* pack, uint32_t packSize, uint32_t poolPages, int exact) {
  if(!pack) {
    pack = g_basePack;
    packSize = g_basePackSize;
  }
  if(!pack || !pack_open(pack, packSize, &g_pack)) return 1;
  if(poolPages == 0 || poolPages > NS_VP_COUNT) poolPages = NS_VP_COUNT;
  free(g_pool);
  g_pool = malloc(poolPages * NS_PAGE_SIZE);
  if(!g_pool) return 2;
  memset(g_fb, 0, sizeof(g_fb));
  snes_init(&g_snes, g_pool, poolPages * NS_PAGE_SIZE, putLine, NULL);
  pack_attach(&g_pack, &g_snes);
  if(exact) g_snes.speedHacks = false;
  snes_reset(&g_snes, true);
  return 0;
}

/* Runs one frame with the given buttons. Returns 1 if RAM ran out. */
int EXPORT(nsw_frame)(uint32_t pad, int sample) {
  snes_setPad(&g_snes, 0, (uint16_t)pad);
  snes_runFrame(&g_snes);
  if(sample && g_prof) prof_sample(g_prof, &g_snes);
  return g_snes.pager.oom;
}

/* Runs frames [start, start + count) with automatic (or recorded) inputs.
 * flags: 1 = record pages in the profiler, 2 = collect after every frame
 * (so that the peak statistic shows what is really needed).
 * Returns the number of frames completed before RAM ran out. */
uint32_t EXPORT(nsw_run)(uint32_t count, uint32_t seed, uint32_t start, int flags) {
  for(uint32_t i = 0; i < count; i++) {
    snes_setPad(&g_snes, 0, inputAt(seed, start + i));
    snes_runFrame(&g_snes);
    if(g_snes.pager.oom) return i;
    if((flags & 1) && g_prof) prof_sample(g_prof, &g_snes);
    if(flags & 2) pager_collect(&g_snes.pager);
  }
  return count;
}

const uint16_t* EXPORT(nsw_fb)(void) { return g_fb; }

/* inputs recorded while someone played: replaces the button masher */
void EXPORT(nsw_movie)(const uint16_t* frames, uint32_t count) {
  g_movie = count ? frames : NULL;
  g_movieLen = count;
}

/* 0 pool pages, 1 pages in use, 2 peak in use, 3 copy-on-writes,
 * 4 collections, 5 given back as zero, 6 given back to the dictionary,
 * 7 out of memory, 8 dictionary entries of the session's pack */
uint32_t EXPORT(nsw_stat)(int id) {
  const Pager* pg = &g_snes.pager;
  switch(id) {
    case 0: return pg->poolPages;
    case 1: return pager_usedPages(pg);
    case 2: return pg->peakUsed;
    case 3: return pg->statCow;
    case 4: return pg->statCollects;
    case 5: return pg->statDedupZero;
    case 6: return pg->statDedupDict;
    case 7: return pg->oom;
    case 8: return pg->dictCount;
    default: return 0;
  }
}

int EXPORT(nsw_collect)(void) { return pager_collect(&g_snes.pager); }

/* ---- profiling and packs ---------------------------------------------- */

int EXPORT(nsw_profile_reset)(void) {
  prof_free(g_prof);
  g_prof = prof_new();
  return g_prof ? 0 : 1;
}

void EXPORT(nsw_profile_seed)(const uint8_t* pack, uint32_t size) {
  if(g_prof) prof_seed(g_prof, pack, size);
}

/* Builds a pack from what was profiled. apuMode / frameSkip: 255 = automatic. */
const uint8_t* EXPORT(nsw_build_pack)(uint32_t maxBytes, uint32_t apuMode, uint32_t frameSkip) {
  NsPackParams params;
  fillParams(&params);
  params.maxPackBytes = maxBytes;
  params.apuMode = (uint8_t)apuMode;
  params.frameSkip = (uint8_t)frameSkip;
  free(g_built);
  g_built = prof_buildPack(g_prof, &params, &g_builtSize, &g_builtStats);
  return g_built;
}

/* 0 size in bytes, 1 frames profiled, 2 candidate pages, 3 dictionary
 * entries, 4 of which found in the ROM, 5 of which stored, 6 left out,
 * 7 dropped while profiling */
uint32_t EXPORT(nsw_pack_stat)(int id) {
  switch(id) {
    case 0: return g_builtSize;
    case 1: return g_builtStats.framesSampled;
    case 2: return g_builtStats.candidates;
    case 3: return g_builtStats.dictEntries;
    case 4: return g_builtStats.dictInRom;
    case 5: return g_builtStats.dictStored;
    case 6: return g_builtStats.dictLeftOut;
    case 7: return g_builtStats.dropped;
    default: return 0;
  }
}

/* Smallest RAM pool (in pages) with which the pack plays `frames` frames of
 * the given inputs without running out. 0 if it fails even unlimited. */
uint32_t EXPORT(nsw_min_pool)(const uint8_t* pack, uint32_t packSize, uint32_t frames, uint32_t seed) {
  uint32_t lo = 1, hi = NS_VP_COUNT;
  /* a first pass that collects all the time gives a floor for the search */
  if(nsw_session(pack, packSize, hi, 0) != 0 || nsw_run(frames, seed, 0, 2) != frames) return 0;
  if(g_snes.pager.peakUsed > lo) lo = g_snes.pager.peakUsed;
  while(lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if(nsw_session(pack, packSize, mid, 0) != 0) return 0;
    if(nsw_run(frames, seed, 0, 0) == frames) hi = mid;
    else lo = mid + 1;
  }
  return hi;
}
