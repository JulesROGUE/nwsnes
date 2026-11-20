/* nwsnes PC side - memory profiler and pack builder (see profiler.h) */
#include "profiler.h"

#include <stdlib.h>
#include <string.h>

#include "../core/pack.h"

/* a content must stay unchanged this many frames before it is recorded */
#define MIN_STABLE 4
/* hard cap on recorded contents (256 bytes each) */
#define MAX_ENTRIES 400000u

typedef struct Entry {
  uint32_t hash;      /* ns_pageHash */
  uint32_t weight;    /* page-frames this content was seen stable */
  uint32_t dataIdx;   /* index of the content in data[] */
} Entry;

struct NsProfiler {
  /* per virtual page */
  uint32_t curHash[NS_VP_COUNT];
  uint32_t age[NS_VP_COUNT];
  int32_t curEntry[NS_VP_COUNT];
  /* recorded contents */
  Entry* entries;
  uint32_t count, cap;
  uint8_t* data; /* count * 256 bytes */
  /* open addressing index: entry number + 1, 0 = empty */
  uint32_t* slots;
  uint32_t slotCount; /* power of two */
  uint32_t frames;
  uint32_t dropped;
};

NsProfiler* prof_new(void) {
  NsProfiler* p = calloc(1, sizeof(NsProfiler));
  if(!p) return NULL;
  p->cap = 1024;
  p->entries = malloc(p->cap * sizeof(Entry));
  p->data = malloc((size_t)p->cap * NS_PAGE_SIZE);
  p->slotCount = 4096;
  p->slots = calloc(p->slotCount, sizeof(uint32_t));
  if(!p->entries || !p->data || !p->slots) {
    prof_free(p);
    return NULL;
  }
  for(unsigned i = 0; i < NS_VP_COUNT; i++) p->curEntry[i] = -1;
  return p;
}

void prof_free(NsProfiler* p) {
  if(!p) return;
  free(p->entries);
  free(p->data);
  free(p->slots);
  free(p);
}

static bool grow(NsProfiler* p) {
  if(p->count < p->cap) return true;
  if(p->cap >= MAX_ENTRIES) return false;
  uint32_t cap = p->cap * 2;
  Entry* e = realloc(p->entries, cap * sizeof(Entry));
  if(!e) return false;
  p->entries = e;
  uint8_t* d = realloc(p->data, (size_t)cap * NS_PAGE_SIZE);
  if(!d) return false;
  p->data = d;
  p->cap = cap;
  if(cap * 2 > p->slotCount) {
    uint32_t n = p->slotCount * 4;
    uint32_t* slots = calloc(n, sizeof(uint32_t));
    if(!slots) return false;
    for(uint32_t i = 0; i < p->count; i++) {
      uint32_t k = p->entries[i].hash & (n - 1);
      while(slots[k]) k = (k + 1) & (n - 1);
      slots[k] = i + 1;
    }
    free(p->slots);
    p->slots = slots;
    p->slotCount = n;
  }
  return true;
}

/* finds or records a content; returns its entry number or -1 */
static int32_t intern(NsProfiler* p, uint32_t hash, const uint8_t* content) {
  uint32_t mask = p->slotCount - 1;
  uint32_t k = hash & mask;
  while(p->slots[k]) {
    uint32_t idx = p->slots[k] - 1;
    if(p->entries[idx].hash == hash &&
       memcmp(p->data + (size_t)p->entries[idx].dataIdx * NS_PAGE_SIZE, content, NS_PAGE_SIZE) == 0) {
      return (int32_t)idx;
    }
    k = (k + 1) & mask;
  }
  if(!grow(p)) {
    p->dropped++;
    return -1;
  }
  /* the index may have been rebuilt */
  mask = p->slotCount - 1;
  k = hash & mask;
  while(p->slots[k]) k = (k + 1) & mask;
  uint32_t idx = p->count++;
  p->entries[idx].hash = hash;
  p->entries[idx].weight = 0;
  p->entries[idx].dataIdx = idx;
  memcpy(p->data + (size_t)idx * NS_PAGE_SIZE, content, NS_PAGE_SIZE);
  p->slots[k] = idx + 1;
  return (int32_t)idx;
}

void prof_seed(NsProfiler* p, const uint8_t* packData, uint32_t packSize) {
  NsPack pack;
  if(!pack_open(packData, packSize, &pack)) return;
  for(uint32_t i = 0; i < pack.dictCount; i++) {
    const uint8_t* e = pack.dict + i * NS_DICT_ENTRY_SIZE;
    const uint8_t* content = pack.base + ns_rd32(e + 4);
    int32_t idx = intern(p, ns_rd32(e), content);
    /* earlier sessions vouched for it: make sure it outranks one-off pages */
    if(idx >= 0 && p->entries[idx].weight < 1000) p->entries[idx].weight = 1000;
  }
}

void prof_sample(NsProfiler* p, const Snes* s) {
  p->frames++;
  for(unsigned vp = 0; vp < NS_VP_COUNT; vp++) {
    const uint8_t* page = s->pager.ptr[vp];
    if(page == ns_zeroPage) {
      p->curHash[vp] = 0;
      p->age[vp] = 0;
      p->curEntry[vp] = -1;
      continue;
    }
    uint32_t h = ns_pageHash(page);
    if(h != p->curHash[vp] || p->age[vp] == 0) {
      p->curHash[vp] = h;
      p->age[vp] = 1;
      p->curEntry[vp] = -1;
      continue;
    }
    p->age[vp]++;
    if(p->age[vp] == MIN_STABLE) {
      if(memcmp(page, ns_zeroPage, NS_PAGE_SIZE) == 0) {
        p->age[vp] = 0; /* all zero: free on the device anyway */
        continue;
      }
      p->curEntry[vp] = intern(p, h, page);
      if(p->curEntry[vp] >= 0) p->entries[p->curEntry[vp]].weight += MIN_STABLE;
    } else if(p->age[vp] > MIN_STABLE && p->curEntry[vp] >= 0) {
      p->entries[p->curEntry[vp]].weight++;
    }
  }
}

uint8_t* ns_padRom(const uint8_t* data, uint32_t length, uint32_t* outSize) {
  uint32_t newLength = 0x8000;
  while(newLength < length) newLength *= 2;
  uint8_t* out = malloc(newLength);
  if(!out) return NULL;
  memcpy(out, data, length);
  /* mirror the way the address lines would (same as LakeSnes) */
  uint32_t test = 1;
  while(length != newLength) {
    if(length & test) {
      memcpy(out + length, out + length - test, test);
      length += test;
    }
    test *= 2;
  }
  *outSize = newLength;
  return out;
}

/* ------------------------------------------------------- pack builder -- */

static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static const NsProfiler* g_sortProf;

static int byWeightDesc(const void* a, const void* b) {
  const Entry* ea = &g_sortProf->entries[*(const uint32_t*)a];
  const Entry* eb = &g_sortProf->entries[*(const uint32_t*)b];
  if(ea->weight != eb->weight) return ea->weight < eb->weight ? 1 : -1;
  if(ea->hash != eb->hash) return ea->hash < eb->hash ? -1 : 1;
  return 0;
}

typedef struct DictOut {
  uint32_t hash;
  uint32_t offset;
} DictOut;

static int byHash(const void* a, const void* b) {
  const DictOut* da = a;
  const DictOut* db = b;
  if(da->hash != db->hash) return da->hash < db->hash ? -1 : 1;
  if(da->offset != db->offset) return da->offset < db->offset ? -1 : 1;
  return 0;
}

/* Rabin-Karp: finds which recorded contents appear verbatim in the ROM, at
 * any byte offset. romPos[i] receives the offset or 0xffffffff. */
#define RK_BASE 0x01000193u
static uint32_t rkHash(const uint8_t* p) {
  uint32_t h = 0;
  for(unsigned i = 0; i < NS_PAGE_SIZE; i++) h = h * RK_BASE + p[i];
  return h;
}

static void findInRom(const NsProfiler* p, const uint8_t* rom, uint32_t romSize, uint32_t* romPos) {
  for(uint32_t i = 0; i < p->count; i++) romPos[i] = 0xffffffffu;
  if(romSize < NS_PAGE_SIZE || p->count == 0) return;
  uint32_t n = 16;
  while(n < p->count * 2) n *= 2;
  uint32_t* slots = calloc(n, sizeof(uint32_t)); /* entry + 1 */
  uint32_t* rk = malloc(p->count * sizeof(uint32_t));
  if(!slots || !rk) {
    free(slots);
    free(rk);
    return;
  }
  for(uint32_t i = 0; i < p->count; i++) {
    rk[i] = rkHash(p->data + (size_t)i * NS_PAGE_SIZE);
    uint32_t k = (rk[i] * 0x9e3779b1u) & (n - 1);
    while(slots[k]) k = (k + 1) & (n - 1);
    slots[k] = i + 1;
  }
  uint32_t pow = 1; /* RK_BASE ^ 255 */
  for(unsigned i = 0; i < NS_PAGE_SIZE - 1; i++) pow *= RK_BASE;
  uint32_t h = rkHash(rom);
  for(uint32_t pos = 0;; pos++) {
    uint32_t k = (h * 0x9e3779b1u) & (n - 1);
    while(slots[k]) {
      uint32_t idx = slots[k] - 1;
      if(rk[idx] == h && romPos[idx] == 0xffffffffu &&
         memcmp(rom + pos, p->data + (size_t)idx * NS_PAGE_SIZE, NS_PAGE_SIZE) == 0) {
        romPos[idx] = pos;
      }
      k = (k + 1) & (n - 1);
    }
    if(pos + NS_PAGE_SIZE >= romSize) break;
    h = (h - rom[pos] * pow) * RK_BASE + rom[pos + NS_PAGE_SIZE];
  }
  free(slots);
  free(rk);
}

void prof_seedRomUse(const uint8_t* packData, uint32_t packSize, uint8_t* romUsed, uint32_t romSize) {
  NsPack pack;
  if(!pack_open(packData, packSize, &pack) || !pack.blockTable || pack.info.romSize != romSize) return;
  for(uint32_t i = 0; i < romSize >> NS_ROM_BLOCK_SHIFT; i++) {
    if(ns_rd32(pack.blockTable + i * 4) != 0xffffffffu) memset(romUsed + i * (NS_ROM_BLOCK / NS_PAGE_SIZE), 1, NS_ROM_BLOCK / NS_PAGE_SIZE);
  }
}

/* picks the dictionary entries that fit: best contents first, until the
 * flash budget is used up. Returns the bytes used, fixed part included. */
static uint32_t pickDict(const NsProfiler* p, uint32_t count, const uint32_t* order, const uint32_t* romPos, uint32_t romOff,
                         uint32_t fixed, uint32_t maxPackBytes, DictOut* dict, uint32_t* stored, uint32_t* nDictOut,
                         uint32_t* nStoredOut, NsPackStats* st) {
  uint32_t nDict = 0, nStored = 0, used = fixed;
  st->dictInRom = 0;
  st->dictLeftOut = 0;
  for(uint32_t i = 0; i < count; i++) {
    uint32_t idx = order[i];
    uint32_t cost = NS_DICT_ENTRY_SIZE + (romPos[idx] == 0xffffffffu ? NS_PAGE_SIZE : 0);
    if(maxPackBytes && used + cost > maxPackBytes) {
      st->dictLeftOut++;
      continue;
    }
    used += cost;
    dict[nDict].hash = p->entries[idx].hash;
    if(romPos[idx] != 0xffffffffu) {
      dict[nDict].offset = romOff + romPos[idx];
      st->dictInRom++;
    } else {
      dict[nDict].offset = 0x80000000u | nStored; /* patched by the caller */
      stored[nStored++] = idx;
    }
    nDict++;
  }
  *nDictOut = nDict;
  *nStoredOut = nStored;
  return used;
}

uint8_t* prof_buildPack(const NsProfiler* p, const NsPackParams* params, uint32_t* outSize, NsPackStats* stats) {
  NsPackStats st;
  memset(&st, 0, sizeof(st));
  st.framesSampled = p ? p->frames : 0;
  st.candidates = p ? p->count : 0;
  st.dropped = p ? p->dropped : 0;
  uint32_t count = p ? p->count : 0;

  /* The ROM goes in whole when it fits. Otherwise, and if it is known what
   * the game reads, only those 4 KB blocks go in (version 2 of the format). */
  const uint32_t nBlocks = params->romSize >> NS_ROM_BLOCK_SHIFT;
  const bool blocks = params->maxPackBytes && params->romUsed && NS_PACK_HEADER_SIZE + params->romSize > params->maxPackBytes;
  const uint32_t headerSize = blocks ? NS_PACK_HEADER_SIZE_BLOCKS : NS_PACK_HEADER_SIZE;
  const uint32_t tableOff = headerSize;
  const uint32_t romOff = headerSize + (blocks ? nBlocks * 4 : 0);
  st.romBlocks = nBlocks;
  st.romBlocksRead = nBlocks;
  st.romBlocksPacked = nBlocks;

  uint32_t* order = malloc((count + 1) * sizeof(uint32_t));
  uint32_t* romPos = malloc((count + 1) * sizeof(uint32_t));
  DictOut* dict = malloc((count + 1) * sizeof(DictOut));
  uint32_t* stored = malloc((count + 1) * sizeof(uint32_t)); /* entries needing their own flash page */
  uint8_t* present = calloc(nBlocks + 1, 1);                  /* blocks that go in the pack */
  uint8_t* image = blocks ? malloc(params->romSize) : NULL;   /* those blocks one after the other */
  if(!order || !romPos || !dict || !stored || !present || (blocks && !image)) {
    free(order); free(romPos); free(dict); free(stored); free(present); free(image);
    return NULL;
  }
  if(blocks) {
    st.romBlocksRead = 0;
    for(uint32_t i = 0; i < nBlocks; i++) {
      for(uint32_t j = 0; j < NS_ROM_BLOCK / NS_PAGE_SIZE; j++) present[i] |= params->romUsed[i * (NS_ROM_BLOCK / NS_PAGE_SIZE) + j];
      present[i] = present[i] != 0;
      st.romBlocksRead += present[i];
    }
  } else {
    memset(present, 1, nBlocks);
  }
  for(uint32_t i = 0; i < count; i++) order[i] = i;
  if(count) {
    g_sortProf = p;
    qsort(order, count, sizeof(uint32_t), byWeightDesc);
  }

  uint32_t nDict = 0, nStored = 0, imageSize = 0;
  const uint8_t* romData = params->rom;
  for(int pass = 0; pass < (blocks ? 2 : 1); pass++) {
    if(blocks) {
      imageSize = 0;
      for(uint32_t i = 0; i < nBlocks; i++) {
        if(!present[i]) continue;
        memcpy(image + imageSize, params->rom + ((size_t)i << NS_ROM_BLOCK_SHIFT), NS_ROM_BLOCK);
        imageSize += NS_ROM_BLOCK;
      }
      romData = image;
    } else {
      imageSize = params->romSize;
    }
    for(uint32_t i = 0; i < count; i++) romPos[i] = 0xffffffffu;
    if(count && imageSize >= NS_PAGE_SIZE) findInRom(p, romData, imageSize, romPos);
    const uint32_t used = pickDict(p, count, order, romPos, romOff, romOff + imageSize, params->maxPackBytes, dict, stored,
                                   &nDict, &nStored, &st);
    if(!blocks || pass == 1) break;
    /* Room left once everything known is in: use it for the blocks next to
     * the ones the game read. A game played on the calculator never does
     * exactly what it did on the PC, and what it reads instead is usually
     * close by (another frame of an animation, the next entry of a table). */
    uint32_t extra = used < params->maxPackBytes ? (params->maxPackBytes - used) / NS_ROM_BLOCK : 0;
    if(extra == 0 || st.romBlocksRead == 0) break;
    uint32_t* dist = malloc(nBlocks * sizeof(uint32_t));
    if(!dist) break;
    uint32_t d = 0xffffffffu;
    for(uint32_t i = 0; i < nBlocks; i++) {
      d = present[i] ? 0 : (d == 0xffffffffu ? d : d + 1);
      dist[i] = d;
    }
    d = 0xffffffffu;
    for(uint32_t i = nBlocks; i-- > 0;) {
      d = present[i] ? 0 : (d == 0xffffffffu ? d : d + 1);
      if(d < dist[i]) dist[i] = d;
    }
    for(uint32_t want = 1; extra > 0 && want < nBlocks; want++) {
      for(uint32_t i = 0; i < nBlocks && extra > 0; i++) {
        if(dist[i] == want) {
          present[i] = 1;
          extra--;
        }
      }
    }
    free(dist);
  }
  st.romBlocksPacked = 0;
  for(uint32_t i = 0; i < nBlocks; i++) st.romBlocksPacked += present[i];

  const uint32_t fixed = romOff + imageSize;
  uint32_t dictOff = fixed;
  uint32_t dataOff = dictOff + nDict * NS_DICT_ENTRY_SIZE;
  uint32_t total = dataOff + nStored * NS_PAGE_SIZE;
  uint8_t* pack = calloc(1, total);
  if(!pack) {
    free(order); free(romPos); free(dict); free(stored); free(present); free(image);
    return NULL;
  }
  for(uint32_t i = 0; i < nDict; i++) {
    if(dict[i].offset & 0x80000000u) dict[i].offset = dataOff + (dict[i].offset & 0x7fffffffu) * NS_PAGE_SIZE;
  }
  qsort(dict, nDict, sizeof(DictOut), byHash);

  memcpy(pack, NS_PACK_MAGIC, 8);
  wr32(pack + 8, blocks ? NS_PACK_VERSION_BLOCKS : NS_PACK_VERSION);
  wr32(pack + 12, headerSize);
  wr32(pack + 16, total);
  wr32(pack + 20, romOff);
  wr32(pack + 24, params->romSize);
  wr32(pack + 28, params->ramSize);
  wr32(pack + 32, dictOff);
  wr32(pack + 36, nDict);
  pack[40] = params->cartType;
  pack[41] = params->pal ? 1 : 0;
  pack[42] = params->apuMode;
  pack[43] = params->frameSkip;
  wr32(pack + 44, blocks && st.romBlocksPacked < nBlocks ? NS_PACK_FLAG_PARTIAL : 0);
  if(params->title) {
    size_t n = strlen(params->title);
    memcpy(pack + 48, params->title, n > 16 ? 16 : n);
  }
  if(blocks) {
    wr32(pack + 64, tableOff);
    wr32(pack + 68, nBlocks);
    wr32(pack + 72, st.romBlocksPacked);
    uint32_t off = romOff;
    for(uint32_t i = 0; i < nBlocks; i++) {
      wr32(pack + tableOff + i * 4, present[i] ? off : 0xffffffffu);
      if(present[i]) off += NS_ROM_BLOCK;
    }
  }
  memcpy(pack + romOff, romData, imageSize);
  for(uint32_t i = 0; i < nDict; i++) {
    wr32(pack + dictOff + i * NS_DICT_ENTRY_SIZE, dict[i].hash);
    wr32(pack + dictOff + i * NS_DICT_ENTRY_SIZE + 4, dict[i].offset);
  }
  for(uint32_t i = 0; i < nStored; i++) {
    memcpy(pack + dataOff + i * NS_PAGE_SIZE, p->data + (size_t)stored[i] * NS_PAGE_SIZE, NS_PAGE_SIZE);
  }
  st.dictEntries = nDict;
  st.dictStored = nStored;
  st.packBytes = total;
  if(stats) *stats = st;
  *outSize = total;
  free(order); free(romPos); free(dict); free(stored); free(present); free(image);
  return pack;
}
