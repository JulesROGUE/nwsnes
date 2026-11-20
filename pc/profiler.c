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
    const uint8_t* content = pack_dictPage(&pack, ns_rd32(e + 4));
    if(!content) continue; /* in an extension that did not come with the file */
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
  bool isStored; /* a page of its own in the pack, as opposed to a place in the ROM */
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
 * flash budget is used up. A content found as is in the ROM blocks of the
 * pack (mainPos) or of its extension (extPos) costs its entry only. Returns
 * the bytes used, fixed part included. */
static uint32_t pickDict(const NsProfiler* p, uint32_t count, const uint32_t* order, const uint32_t* mainPos,
                         const uint32_t* extPos, uint32_t romOff, uint32_t fixed, uint32_t maxPackBytes, DictOut* dict,
                         uint32_t* stored, uint32_t* nDictOut, uint32_t* nStoredOut, NsPackStats* st) {
  uint32_t nDict = 0, nStored = 0, used = fixed;
  st->dictInRom = 0;
  st->dictLeftOut = 0;
  for(uint32_t i = 0; i < count; i++) {
    uint32_t idx = order[i];
    const bool inRom = mainPos[idx] != 0xffffffffu || extPos[idx] != 0xffffffffu;
    uint32_t cost = NS_DICT_ENTRY_SIZE + (inRom ? 0 : NS_PAGE_SIZE);
    if(maxPackBytes && used + cost > maxPackBytes) {
      st->dictLeftOut++;
      continue;
    }
    used += cost;
    dict[nDict].hash = p->entries[idx].hash;
    dict[nDict].isStored = false;
    if(mainPos[idx] != 0xffffffffu) {
      dict[nDict].offset = romOff + mainPos[idx];
      st->dictInRom++;
    } else if(extPos[idx] != 0xffffffffu) {
      dict[nDict].offset = NS_PACK_IN_EXT | (NS_PACK_EXT_HEADER_SIZE + extPos[idx]);
      st->dictInRom++;
    } else {
      dict[nDict].offset = nStored; /* its rank among the stored pages: turned into an offset by the caller */
      dict[nDict].isStored = true;
      stored[nStored++] = idx;
    }
    nDict++;
  }
  *nDictOut = nDict;
  *nStoredOut = nStored;
  return used;
}

/* marks the blocks nearest to the ones already taken, as many as "extra" */
static void takeNeighbours(uint8_t* present, uint32_t nBlocks, uint32_t extra) {
  uint32_t* dist = malloc(nBlocks * sizeof(uint32_t));
  if(!dist) return;
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

/* A block is stored once, however many times its content appears in the ROM
 * (a 3 MB ROM comes mirrored to 4 MB, and games have a few empty blocks):
 * first[i] is the first block with the content of block i. Returns the
 * number of distinct contents. */
static uint32_t findTwins(const uint8_t* rom, uint32_t nBlocks, uint32_t* first) {
  uint32_t* hash = malloc((nBlocks + 1) * sizeof(uint32_t));
  uint32_t distinct = 0;
  for(uint32_t i = 0; i < nBlocks; i++) {
    const uint8_t* b = rom + ((size_t)i << NS_ROM_BLOCK_SHIFT);
    const uint32_t h = ns_hashBytes(b, NS_ROM_BLOCK);
    if(hash) hash[i] = h;
    first[i] = i;
    for(uint32_t j = 0; hash && j < i; j++) {
      if(hash[j] == h && first[j] == j && memcmp(rom + ((size_t)j << NS_ROM_BLOCK_SHIFT), b, NS_ROM_BLOCK) == 0) {
        first[i] = j;
        break;
      }
    }
    distinct += first[i] == i;
  }
  free(hash);
  return distinct;
}

/* a content that is in the pack serves all the blocks that have it */
static void spreadToTwins(uint8_t* present, const uint32_t* first, uint32_t nBlocks) {
  for(uint32_t i = 0; i < nBlocks; i++) {
    if(present[i]) present[first[i]] = 1;
  }
  for(uint32_t i = 0; i < nBlocks; i++) present[i] = present[first[i]];
}

uint8_t* prof_buildPack(const NsProfiler* p, const NsPackParams* params, uint32_t* outSize, NsPackStats* stats) {
  NsPackStats st;
  memset(&st, 0, sizeof(st));
  st.framesSampled = p ? p->frames : 0;
  st.candidates = p ? p->count : 0;
  st.dropped = p ? p->dropped : 0;
  uint32_t count = p ? p->count : 0;

  /* Three layouts, by decreasing comfort:
   *  - the ROM fits in the pack: it goes in whole, in one piece (version 1);
   *  - it fits with the help of the extension (the app area of the other
   *    half of the flash): it goes in whole, as 4 KB blocks. The extension
   *    takes all the blocks it can, starting from the end of the ROM: what
   *    it holds then depends on the ROM alone, not on how the game was
   *    played, and a pack rebuilt after more play goes with the extension
   *    already on the calculator;
   *  - it does not fit at all: only the blocks the game was seen to read go
   *    in, plus their neighbours as far as room allows. */
  const uint32_t nBlocks = params->romSize >> NS_ROM_BLOCK_SHIFT;
  const bool blocks = params->maxPackBytes && NS_PACK_HEADER_SIZE + params->romSize > params->maxPackBytes &&
                      (params->romUsed || params->maxExtBytes);
  const uint32_t extRoom = blocks && params->maxExtBytes > NS_PACK_EXT_HEADER_SIZE
                               ? (params->maxExtBytes - NS_PACK_EXT_HEADER_SIZE) >> NS_ROM_BLOCK_SHIFT : 0;
  const uint32_t headerSize = !blocks ? NS_PACK_HEADER_SIZE : (extRoom ? NS_PACK_HEADER_SIZE_EXT : NS_PACK_HEADER_SIZE_BLOCKS);
  const uint32_t tableOff = headerSize;
  const uint32_t romOff = headerSize + (blocks ? nBlocks * 4 : 0);
  st.romBlocks = nBlocks;
  st.romBlocksRead = nBlocks;
  st.romBlocksPacked = nBlocks;

  uint32_t* order = malloc((count + 1) * sizeof(uint32_t));
  uint32_t* mainPos = malloc((count + 1) * sizeof(uint32_t));
  uint32_t* extPos = malloc((count + 1) * sizeof(uint32_t));
  DictOut* dict = malloc((count + 1) * sizeof(DictOut));
  uint32_t* stored = malloc((count + 1) * sizeof(uint32_t)); /* entries needing their own flash page */
  uint8_t* present = calloc(nBlocks + 1, 1);                  /* blocks that go in the pack or its extension */
  uint8_t* inExt = calloc(nBlocks + 1, 1);                    /* ...of which in the extension */
  uint8_t* wasRead = calloc(nBlocks + 1, 1);
  uint32_t* first = malloc((nBlocks + 1) * sizeof(uint32_t)); /* the block whose content block i shares */
  uint32_t* where = malloc((nBlocks + 1) * sizeof(uint32_t)); /* block table */
  uint8_t* image = blocks ? malloc(params->romSize) : NULL;    /* the blocks of the pack one after the other */
  uint8_t* extImage = blocks ? malloc(params->romSize) : NULL; /* the same for the extension */
#define FREE_ALL() \
  do { \
    free(order); free(mainPos); free(extPos); free(dict); free(stored); free(present); free(inExt); free(wasRead); \
    free(first); free(where); free(image); free(extImage); \
  } while(0)
  if(!order || !mainPos || !extPos || !dict || !stored || !present || !inExt || !wasRead || !first || !where ||
     (blocks && (!image || !extImage))) {
    FREE_ALL();
    return NULL;
  }
  bool whole = true;
  if(blocks) {
    st.romBlocksRead = 0;
    for(uint32_t i = 0; i < nBlocks; i++) {
      if(params->romUsed) {
        for(uint32_t j = 0; j < NS_ROM_BLOCK / NS_PAGE_SIZE; j++) wasRead[i] |= params->romUsed[i * (NS_ROM_BLOCK / NS_PAGE_SIZE) + j];
      }
      wasRead[i] = wasRead[i] != 0;
      st.romBlocksRead += wasRead[i];
    }
    /* whole ROM: what the extension cannot take must fit in the pack */
    const uint32_t distinct = findTwins(params->rom, nBlocks, first);
    const uint32_t inMain = distinct > extRoom ? distinct - extRoom : 0;
    whole = romOff + (uint64_t)inMain * NS_ROM_BLOCK <= params->maxPackBytes;
    if(!whole && !params->romUsed) whole = true; /* nothing known about what is read: all of it, too big or not */
    for(uint32_t i = 0; i < nBlocks; i++) present[i] = whole || wasRead[i];
    spreadToTwins(present, first, nBlocks);
  } else {
    memset(present, 1, nBlocks);
  }
  for(uint32_t i = 0; i < count; i++) order[i] = i;
  if(count) {
    g_sortProf = p;
    qsort(order, count, sizeof(uint32_t), byWeightDesc);
  }

  uint32_t nDict = 0, nStored = 0, imageSize = 0, extBlocks = 0;
  const uint8_t* romData = params->rom;
  for(int pass = 0; pass < (blocks ? 2 : 1); pass++) {
    for(uint32_t i = 0; i < count; i++) mainPos[i] = extPos[i] = 0xffffffffu;
    if(blocks) {
      /* the extension takes the last blocks, as many as it can hold */
      uint32_t toStore = 0;
      for(uint32_t i = 0; i < nBlocks; i++) toStore += present[i] && first[i] == i;
      extBlocks = toStore < extRoom ? toStore : extRoom;
      memset(inExt, 0, nBlocks);
      for(uint32_t i = nBlocks, left = extBlocks; i-- > 0 && left > 0;) {
        if(present[i] && first[i] == i) {
          inExt[i] = 1;
          left--;
        }
      }
      imageSize = 0;
      uint32_t extImageSize = 0;
      for(uint32_t i = 0; i < nBlocks; i++) {
        if(!present[i] || first[i] != i) continue;
        const uint8_t* b = params->rom + ((size_t)i << NS_ROM_BLOCK_SHIFT);
        if(inExt[i]) {
          memcpy(extImage + extImageSize, b, NS_ROM_BLOCK);
          extImageSize += NS_ROM_BLOCK;
        } else {
          memcpy(image + imageSize, b, NS_ROM_BLOCK);
          imageSize += NS_ROM_BLOCK;
        }
      }
      romData = image;
      if(count && extImageSize >= NS_PAGE_SIZE) findInRom(p, extImage, extImageSize, extPos);
    } else {
      imageSize = params->romSize;
    }
    if(count && imageSize >= NS_PAGE_SIZE) findInRom(p, romData, imageSize, mainPos);
    const uint32_t used = pickDict(p, count, order, mainPos, extPos, romOff, romOff + imageSize, params->maxPackBytes, dict,
                                   stored, &nDict, &nStored, &st);
    if(!blocks || whole || pass == 1) break;
    /* Part of the ROM only, and room left once everything known is in: use
     * it for the blocks next to the ones the game read. A game played on the
     * calculator never does exactly what it did on the PC, and what it reads
     * instead is usually close by (another frame of an animation, the next
     * entry of a table). */
    uint32_t extra = extRoom - extBlocks;
    if(used < params->maxPackBytes) extra += (params->maxPackBytes - used) / NS_ROM_BLOCK;
    if(extra == 0 || st.romBlocksRead == 0) break;
    takeNeighbours(present, nBlocks, extra);
    spreadToTwins(present, first, nBlocks);
  }
  st.romBlocksPacked = 0;
  for(uint32_t i = 0; i < nBlocks; i++) st.romBlocksPacked += present[i];
  st.romBlocksInExt = extBlocks;

  const uint32_t fixed = romOff + imageSize;
  uint32_t dictOff = fixed;
  uint32_t dataOff = dictOff + nDict * NS_DICT_ENTRY_SIZE;
  uint32_t total = dataOff + nStored * NS_PAGE_SIZE;
  const uint32_t extSize = extBlocks ? NS_PACK_EXT_HEADER_SIZE + extBlocks * NS_ROM_BLOCK : 0;
  uint8_t* pack = calloc(1, (size_t)total + extSize);
  if(!pack) {
    FREE_ALL();
    return NULL;
  }
  for(uint32_t i = 0; i < nDict; i++) {
    if(dict[i].isStored) dict[i].offset = dataOff + dict[i].offset * NS_PAGE_SIZE;
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
  memcpy(pack + romOff, romData, imageSize);
  if(blocks) {
    wr32(pack + 64, tableOff);
    wr32(pack + 68, nBlocks);
    wr32(pack + 72, st.romBlocksPacked);
    wr32(pack + 76, extSize);
    uint8_t* ext = pack + total;
    uint32_t off = romOff, extOff = NS_PACK_EXT_HEADER_SIZE;
    for(uint32_t i = 0; i < nBlocks; i++) {
      if(!present[i]) {
        where[i] = 0xffffffffu;
      } else if(first[i] != i) {
        where[i] = where[first[i]]; /* first[i] < i: already placed */
      } else if(inExt[i]) {
        where[i] = NS_PACK_IN_EXT | extOff;
        memcpy(ext + extOff, params->rom + ((size_t)i << NS_ROM_BLOCK_SHIFT), NS_ROM_BLOCK);
        extOff += NS_ROM_BLOCK;
      } else {
        where[i] = off;
        off += NS_ROM_BLOCK;
      }
      wr32(pack + tableOff + i * 4, where[i]);
    }
    if(extSize) {
      /* The identifier is derived from what the extension holds, and from
       * nothing else: any pack that wants these blocks in this order goes
       * with it. */
      const uint32_t hash = ns_hashBytes(ext + NS_PACK_EXT_HEADER_SIZE, extSize - NS_PACK_EXT_HEADER_SIZE);
      const uint32_t id = hash ^ 0x4e575358u;
      memcpy(ext, NS_PACK_EXT_MAGIC, 8);
      wr32(ext + 8, extSize);
      wr32(ext + 12, id);
      wr32(pack + 80, id);
      wr32(pack + 84, hash);
    }
  }
  for(uint32_t i = 0; i < nDict; i++) {
    wr32(pack + dictOff + i * NS_DICT_ENTRY_SIZE, dict[i].hash);
    wr32(pack + dictOff + i * NS_DICT_ENTRY_SIZE + 4, dict[i].offset);
  }
  for(uint32_t i = 0; i < nStored; i++) {
    memcpy(pack + dataOff + i * NS_PAGE_SIZE, p->data + (size_t)stored[i] * NS_PAGE_SIZE, NS_PAGE_SIZE);
  }
  st.dictEntries = nDict;
  st.dictStored = nStored;
  st.packBytes = total + extSize;
  st.mainBytes = total;
  st.extBytes = extSize;
  if(stats) *stats = st;
  *outSize = total + extSize;
  FREE_ALL();
#undef FREE_ALL
  return pack;
}
