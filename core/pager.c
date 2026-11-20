/* nwsnes - content-addressed paging (see pager.h) */
#include "pager.h"

#include <string.h>

const uint8_t ns_zeroPage[NS_PAGE_SIZE] = {0};

/* Hashes a page and tells whether it is all zero. Words are read little
 * endian whatever the host, so hashes computed on the PC match the device. */
static uint32_t hashPage(const uint8_t* p, bool* isZero) {
  uint32_t h = 0x9e3779b9u;
  uint32_t acc = 0;
  for(unsigned i = 0; i < NS_PAGE_SIZE; i += 4) {
    uint32_t w = ns_rd32(p + i);
    acc |= w;
    h ^= w;
    h *= 0x85ebca6bu;
    h = (h << 13) | (h >> 19);
  }
  h ^= h >> 16;
  h *= 0xc2b2ae35u;
  h ^= h >> 13;
  if(isZero) *isZero = acc == 0;
  return h;
}

uint32_t ns_pageHash(const uint8_t* page) {
  return hashPage(page, NULL);
}

static void freeAll(Pager* pg) {
  pg->freeHead = NS_NO_PAGE;
  pg->freeCount = 0;
  for(int i = (int)pg->poolPages - 1; i >= 0; i--) {
    uint8_t* p = pg->pool + ((uint32_t)i << NS_PAGE_SHIFT);
    p[0] = (uint8_t)(pg->freeHead & 0xff);
    p[1] = (uint8_t)(pg->freeHead >> 8);
    pg->freeHead = (uint16_t)i;
    pg->freeCount++;
  }
}

void pager_init(Pager* pg, uint8_t* pool, uint32_t poolBytes) {
  memset(pg, 0, sizeof(*pg));
  uint32_t pages = poolBytes >> NS_PAGE_SHIFT;
  if(pages > NS_VP_COUNT) pages = NS_VP_COUNT; /* more can never be used */
  pg->pool = pool;
  pg->poolPages = (uint16_t)pages;
  pg->poolBytes = pages << NS_PAGE_SHIFT;
  pg->lowWater = (uint16_t)(pages / 8 < 4 ? 4 : pages / 8);
  pager_reset(pg);
}

void* pager_carve(Pager* pg, uint32_t bytes) {
  const uint32_t pages = (bytes + NS_PAGE_SIZE - 1) >> NS_PAGE_SHIFT;
  if(pages + 4 > pg->poolPages) return NULL;
  pg->poolPages = (uint16_t)(pg->poolPages - pages);
  pg->poolBytes = (uint32_t)pg->poolPages << NS_PAGE_SHIFT;
  pg->lowWater = (uint16_t)(pg->poolPages / 8 < 4 ? 4 : pg->poolPages / 8);
  pager_reset(pg);
  return pg->pool + pg->poolBytes;
}

void pager_setDictionary(Pager* pg, const uint8_t* packBase, const uint8_t* extBase, const uint8_t* dict, uint32_t count) {
  pg->packBase = packBase;
  pg->extBase = extBase;
  pg->dict = dict;
  pg->dictCount = dict ? count : 0;
}

void pager_reset(Pager* pg) {
  for(unsigned i = 0; i < NS_VP_COUNT; i++) pg->ptr[i] = ns_zeroPage;
  freeAll(pg);
  pg->minFree = pg->freeCount;
  pg->peakUsed = 0;
  pg->oom = false;
}

static void freePage(Pager* pg, const uint8_t* ram) {
  uint8_t* p = (uint8_t*)ram;
  uint16_t idx = (uint16_t)(((uintptr_t)p - (uintptr_t)pg->pool) >> NS_PAGE_SHIFT);
  p[0] = (uint8_t)(pg->freeHead & 0xff);
  p[1] = (uint8_t)(pg->freeHead >> 8);
  pg->freeHead = idx;
  pg->freeCount++;
}

/* returns the flash copy of this content, or NULL */
static const uint8_t* dictFind(const Pager* pg, uint32_t hash, const uint8_t* content) {
  uint32_t lo = 0, hi = pg->dictCount;
  while(lo < hi) { /* first entry with entry.hash >= hash */
    uint32_t mid = (lo + hi) >> 1;
    if(ns_rd32(pg->dict + mid * NS_DICT_ENTRY_SIZE) < hash) lo = mid + 1;
    else hi = mid;
  }
  for(; lo < pg->dictCount; lo++) {
    const uint8_t* e = pg->dict + lo * NS_DICT_ENTRY_SIZE;
    if(ns_rd32(e) != hash) break;
    const uint32_t off = ns_rd32(e + 4);
    const uint8_t* cand;
    if(!(off & NS_DICT_IN_EXT)) cand = pg->packBase + off;
    else if(pg->extBase) cand = pg->extBase + (off & ~NS_DICT_IN_EXT);
    else continue;
    if(memcmp(cand, content, NS_PAGE_SIZE) == 0) return cand;
  }
  return NULL;
}

int pager_collect(Pager* pg) {
  int freed = 0;
  pg->statCollects++;
  for(unsigned vp = 0; vp < NS_VP_COUNT; vp++) {
    const uint8_t* p = pg->ptr[vp];
    if(!pager_isRam(pg, p)) continue;
    bool zero;
    uint32_t h = hashPage(p, &zero);
    const uint8_t* flash = NULL;
    if(zero) {
      flash = ns_zeroPage;
      pg->statDedupZero++;
    } else if(pg->dictCount) {
      flash = dictFind(pg, h, p);
      if(flash) pg->statDedupDict++;
    }
    if(flash) {
      pg->ptr[vp] = flash;
      freePage(pg, p);
      freed++;
    }
  }
  if(pg->freeCount < pg->minFree) pg->minFree = pg->freeCount;
  uint16_t used = (uint16_t)(pg->poolPages - pg->freeCount);
  if(used > pg->peakUsed) pg->peakUsed = used;
  return freed;
}

void pager_frameEnd(Pager* pg) {
  if(pg->freeCount < pg->lowWater) pager_collect(pg);
}

bool pager_writeSlow(Pager* pg, uint32_t vpage, uint32_t off, uint8_t val) {
  const uint8_t* src = pg->ptr[vpage];
  if(src[off] == val) return true; /* silent store: the page can stay shared */
  if(pg->freeHead == NS_NO_PAGE) {
    /* vpage itself is read-only, so collecting cannot move it */
    pager_collect(pg);
    if(pg->freeHead == NS_NO_PAGE) {
      pg->oom = true;
      return false;
    }
  }
  uint8_t* dst = pg->pool + ((uint32_t)pg->freeHead << NS_PAGE_SHIFT);
  pg->freeHead = (uint16_t)(dst[0] | (dst[1] << 8));
  pg->freeCount--;
  memcpy(dst, src, NS_PAGE_SIZE);
  dst[off] = val;
  pg->ptr[vpage] = dst;
  pg->statCow++;
  return true;
}
