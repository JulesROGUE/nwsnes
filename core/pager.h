/*
 * nwsnes - content-addressed paging
 *
 * The SNES has 128 KB of work RAM and 64 KB of video RAM. A NumWorks external
 * app gets roughly 100 KB in total, so the emulated memory cannot simply be
 * backed by RAM. Instead, every emulated memory space is cut in 256-byte
 * virtual pages and each page points to ONE of three kinds of backing store:
 *
 *   - the shared zero page (flash, read-only) while the page is all zeroes;
 *   - a page of the "dictionary" stored in flash (read-only). The dictionary
 *     is built on a PC by profiling the game (see snes2nw) and holds page
 *     contents the game is known to produce (decompressed graphics, level
 *     maps, tile maps...). Entries may also point straight into the ROM image;
 *   - a page of the RAM pool, the only writable kind.
 *
 * Writes to a read-only page are copy-on-write. A collector periodically
 * hashes RAM pages and, when the content is all zero or is found in the
 * dictionary, points the virtual page back to flash and frees the RAM page.
 * RAM therefore only holds what is both live and unpredictable.
 */
#ifndef NWSNES_PAGER_H
#define NWSNES_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PAGE_SHIFT 8
#define NS_PAGE_SIZE 256u
#define NS_PAGE_MASK 255u

/* virtual page numbers of each emulated memory space */
#define NS_VP_WRAM 0u    /* 128 KB -> 512 pages */
#define NS_VP_VRAM 512u  /*  64 KB -> 256 pages */
#define NS_VP_SRAM 768u  /* up to 32 KB -> 128 pages */
#define NS_VP_COUNT 896u

#define NS_NO_PAGE 0xffffu

/* A dictionary entry is 8 bytes, little endian, possibly unaligned in flash:
 *   u32 hash    ns_pageHash() of the page content
 *   u32 offset  byte offset of the 256-byte content inside the pack */
#define NS_DICT_ENTRY_SIZE 8u

typedef struct Pager {
  const uint8_t* ptr[NS_VP_COUNT]; /* current backing of every virtual page */
  uint8_t* pool;                   /* RAM pool, poolPages * 256 bytes */
  uint32_t poolBytes;
  uint16_t poolPages;
  uint16_t freeCount;
  uint16_t freeHead;       /* head of the free list (index in pool) or NS_NO_PAGE */
  uint16_t lowWater;       /* collect at frame end when freeCount drops below */
  uint16_t minFree;        /* statistics: lowest freeCount ever seen after collection */
  uint16_t peakUsed;       /* statistics: highest number of pool pages in use */
  /* dictionary (in flash), sorted by hash */
  const uint8_t* packBase;
  const uint8_t* dict; /* dictCount entries of NS_DICT_ENTRY_SIZE bytes */
  uint32_t dictCount;
  /* statistics */
  uint32_t statCow;        /* copy-on-write allocations */
  uint32_t statCollects;   /* collector runs */
  uint32_t statDedupZero;  /* pages given back because they were all zero */
  uint32_t statDedupDict;  /* pages given back because found in the dictionary */
  bool oom;                /* set when an allocation could not be satisfied */
} Pager;

extern const uint8_t ns_zeroPage[NS_PAGE_SIZE];

/* pool must stay valid; poolBytes is rounded down to whole pages. */
void pager_init(Pager* pg, uint8_t* pool, uint32_t poolBytes);
void pager_setDictionary(Pager* pg, const uint8_t* packBase, const uint8_t* dict, uint32_t count);
/* points every virtual page back to the zero page and frees the whole pool */
void pager_reset(Pager* pg);
/* gives back every RAM page whose content is zero or known; returns pages freed */
int pager_collect(Pager* pg);
/* called once per frame: collects only when the pool is getting low */
void pager_frameEnd(Pager* pg);
/* slow path of pager_write: copy-on-write. Returns false when out of memory. */
bool pager_writeSlow(Pager* pg, uint32_t vpage, uint32_t off, uint8_t val);
uint32_t ns_pageHash(const uint8_t* page);
static inline bool pager_isRam(const Pager* pg, const uint8_t* p) {
  return (uintptr_t)p - (uintptr_t)pg->pool < pg->poolBytes;
}
static inline uint32_t ns_rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t ns_rd16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
/* number of pool pages currently backing virtual pages */
static inline uint32_t pager_usedPages(const Pager* pg) { return (uint32_t)pg->poolPages - pg->freeCount; }

static inline uint8_t pager_read(const Pager* pg, uint32_t vpage, uint32_t off) {
  return pg->ptr[vpage][off];
}

static inline const uint8_t* pager_page(const Pager* pg, uint32_t vpage) {
  return pg->ptr[vpage];
}

static inline void pager_write(Pager* pg, uint32_t vpage, uint32_t off, uint8_t val) {
  uint8_t* p = (uint8_t*)pg->ptr[vpage];
  if((uintptr_t)p - (uintptr_t)pg->pool < pg->poolBytes) {
    p[off] = val;
    return;
  }
  pager_writeSlow(pg, vpage, off, val);
}

#endif
