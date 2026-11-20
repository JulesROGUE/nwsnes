/*
 * nwsnes host harness - runs the calculator core on a PC, without a window.
 *
 * It exists to answer, with numbers, the questions that cannot be answered
 * on the calculator itself:
 *   - is the fast renderer pixel-identical to the reference one?
 *   - is the paged memory invisible to the game (same RAM, same picture)?
 *   - how much RAM does a game need once its dictionary is in flash?
 *
 * nwsnes-host <rom.sfc | game.pack> [options]      (see usage() below)
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/pack.h"
#include "../core/snes.h"
#include "../pc/profiler.h"
#include "../pc/romhdr.h"

#define FB_H NS_SCREEN_H_OVERSCAN

typedef struct Session {
  Snes snes;
  uint8_t* pool;
  uint32_t poolBytes;
  uint16_t fb[NS_SCREEN_W * FB_H];
} Session;

static void putLine(void* user, int y, const uint16_t* line) {
  Session* se = user;
  if(y >= 0 && y < FB_H) memcpy(se->fb + y * NS_SCREEN_W, line, NS_SCREEN_W * sizeof(uint16_t));
}

static uint8_t* loadFile(const char* path, uint32_t* size) {
  FILE* f = fopen(path, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = malloc(n > 0 ? (size_t)n : 1);
  if(data && fread(data, 1, (size_t)n, f) != (size_t)n) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *size = (uint32_t)n;
  return data;
}

static bool saveFile(const char* path, const uint8_t* data, uint32_t size) {
  FILE* f = fopen(path, "wb");
  if(!f) return false;
  bool ok = fwrite(data, 1, size, f) == size;
  fclose(f);
  return ok;
}

static void savePpm(const char* path, const uint16_t* fb) {
  FILE* f = fopen(path, "wb");
  if(!f) return;
  fprintf(f, "P6\n%d %d\n255\n", NS_SCREEN_W, NS_SCREEN_H);
  for(int i = 0; i < NS_SCREEN_W * NS_SCREEN_H; i++) {
    uint16_t c = fb[i];
    uint8_t rgb[3] = {
      (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)
    };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static uint32_t fnv(const void* data, size_t n, uint32_t h) {
  const uint8_t* p = data;
  for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

/* ---------------------------------------------------------------- input -- */

static uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

/* Deterministic "button masher": holds a random set of buttons for a third
 * of a second, taps Start now and then to get through title screens. */
static uint16_t autoInput(uint32_t seed, uint32_t frame) {
  if(seed == 0) return 0;
  uint32_t block = frame / 20;
  uint32_t r = mix(seed * 0x9e3779b9u + block);
  uint16_t pad = 0;
  if((r & 7) == 0) return (frame % 20) < 4 ? NS_BTN_START : 0;
  if((r & 7) == 1) return 0;
  switch((r >> 3) & 7) {
    case 0: pad |= NS_BTN_LEFT; break;
    case 1: case 2: case 3: pad |= NS_BTN_RIGHT; break;
    case 4: pad |= NS_BTN_UP; break;
    case 5: pad |= NS_BTN_DOWN; break;
    default: break;
  }
  if(r & 0x100) pad |= NS_BTN_B;
  if(r & 0x200) pad |= NS_BTN_Y;
  if((r & 0xc00) == 0xc00) pad |= NS_BTN_A;
  if((r & 0x3000) == 0x3000) pad |= NS_BTN_X;
  if((r & 0x1c000) == 0x1c000) pad |= NS_BTN_L;
  if((r & 0xe0000) == 0xe0000) pad |= NS_BTN_R;
  /* release B a little before the end of the block so jumps can repeat */
  if((frame % 20) >= 16) pad &= (uint16_t)~NS_BTN_B;
  return pad;
}

typedef struct Inputs {
  uint32_t seed;
  uint16_t* script; /* one entry per frame, or NULL */
  uint32_t scriptLen;
} Inputs;

static uint16_t inputAt(const Inputs* in, uint32_t frame) {
  if(in->script) return frame < in->scriptLen ? in->script[frame] : 0;
  return autoInput(in->seed, frame);
}

/* script file: lines of "<first frame> <hex buttons>", held until the next line */
static bool loadScript(const char* path, Inputs* in, uint32_t frames) {
  FILE* f = fopen(path, "r");
  if(!f) return false;
  in->script = calloc(frames + 1, sizeof(uint16_t));
  in->scriptLen = frames;
  uint32_t from = 0, at;
  unsigned buttons, cur = 0;
  while(fscanf(f, "%u %x", &at, &buttons) == 2) {
    for(uint32_t i = from; i < at && i < frames; i++) in->script[i] = (uint16_t)cur;
    from = at;
    cur = buttons;
  }
  for(uint32_t i = from; i < frames; i++) in->script[i] = (uint16_t)cur;
  fclose(f);
  return true;
}

/* -------------------------------------------------------------- session -- */

static bool g_exact; /* --exact: no speed-ups in the device configuration */

static Session* sessionNew(const NsPack* pack, uint32_t poolPages, bool refPpu) {
  Session* se = calloc(1, sizeof(Session));
  se->poolBytes = poolPages * NS_PAGE_SIZE;
  se->pool = malloc(se->poolBytes ? se->poolBytes : 1);
  snes_init(&se->snes, se->pool, se->poolBytes, putLine, se);
  pack_attach(pack, &se->snes);
  se->snes.refPpu = refPpu;
  /* the reference session never uses speed-ups */
  if(refPpu || g_exact) se->snes.speedHacks = false;
  snes_reset(&se->snes, true);
  return se;
}

static void sessionFree(Session* se) {
  if(!se) return;
  free(se->pool);
  free(se);
}

static bool sameMemory(const Session* a, const Session* b, unsigned* where) {
  for(unsigned vp = 0; vp < NS_VP_COUNT; vp++) {
    const uint8_t* pa = a->snes.pager.ptr[vp];
    const uint8_t* pb = b->snes.pager.ptr[vp];
    if(pa != pb && memcmp(pa, pb, NS_PAGE_SIZE) != 0) {
      *where = vp;
      return false;
    }
  }
  return true;
}

static const char* spaceName(unsigned vp) {
  return vp < NS_VP_VRAM ? "WRAM" : (vp < NS_VP_SRAM ? "VRAM" : "SRAM");
}

/* runs the device configuration alone; returns false when it ran out of RAM */
static bool runDevice(const NsPack* pack, const Inputs* in, uint32_t frames, uint32_t poolPages,
                      uint32_t* peakUsed, uint32_t* oomFrame) {
  Session* se = sessionNew(pack, poolPages, false);
  bool ok = true;
  for(uint32_t f = 0; f < frames; f++) {
    snes_setPad(&se->snes, 0, inputAt(in, f));
    snes_runFrame(&se->snes);
    if(se->snes.pager.oom) {
      ok = false;
      if(oomFrame) *oomFrame = f;
      break;
    }
  }
  /* the pool only shows its high-water mark right after a collection */
  pager_collect(&se->snes.pager);
  if(peakUsed) *peakUsed = se->snes.pager.peakUsed;
  sessionFree(se);
  return ok;
}

static void usage(void) {
  fprintf(stderr,
    "usage: nwsnes-host <rom.sfc | game.pack> [options]\n"
    "  --frames N          frames to run (default 600)\n"
    "  --seed N            button masher seed, 0 = no input (default 0)\n"
    "  --input FILE        input script: lines of '<frame> <hex buttons>'\n"
    "  --pool-kb N         RAM pool of the device configuration (default: unlimited)\n"
    "  --ref               draw with the reference renderer\n"
    "  --compare           run reference and device side by side, compare every frame\n"
    "  --dump DIR          write frames as PPM into DIR\n"
    "  --dump-every N      ...one frame out of N (default 60)\n"
    "  --hash              print a hash of the last frame and of the whole run\n"
    "  --hash-each         print a hash of every frame\n"
    "  --profile OUT       profile the game and write a pack to OUT\n"
    "  --budget-kb N       flash budget of that pack (default 2304)\n"
    "  --min-pool          find the smallest RAM pool that completes the run\n"
    "  --collect-each      device: collect after every frame (measures true need)\n"
    "  --apu N             force the audio stand-in mode (0-3)\n"
    "  --exact             device: turn the timing speed-ups off\n");
}

int main(int argc, char** argv) {
  if(argc < 2) {
    usage();
    return 2;
  }
  const char* path = argv[1];
  uint32_t frames = 600, seed = 0, poolKb = 0, dumpEvery = 60, budgetKb = 2304;
  const char *inputPath = NULL, *dumpDir = NULL, *profileOut = NULL;
  bool ref = false, compare = false, hash = false, minPool = false, collectEach = false, hashEach = false;
  int apuMode = -1;
  for(int i = 2; i < argc; i++) {
    const char* a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : "";
    if(!strcmp(a, "--frames")) frames = (uint32_t)strtoul(v, NULL, 0), i++;
    else if(!strcmp(a, "--seed")) seed = (uint32_t)strtoul(v, NULL, 0), i++;
    else if(!strcmp(a, "--input")) inputPath = v, i++;
    else if(!strcmp(a, "--pool-kb")) poolKb = (uint32_t)strtoul(v, NULL, 0), i++;
    else if(!strcmp(a, "--ref")) ref = true;
    else if(!strcmp(a, "--compare")) compare = true;
    else if(!strcmp(a, "--dump")) dumpDir = v, i++;
    else if(!strcmp(a, "--dump-every")) dumpEvery = (uint32_t)strtoul(v, NULL, 0), i++;
    else if(!strcmp(a, "--hash")) hash = true;
    else if(!strcmp(a, "--hash-each")) hashEach = true;
    else if(!strcmp(a, "--profile")) profileOut = v, i++;
    else if(!strcmp(a, "--budget-kb")) budgetKb = (uint32_t)strtoul(v, NULL, 0), i++;
    else if(!strcmp(a, "--min-pool")) minPool = true;
    else if(!strcmp(a, "--collect-each")) collectEach = true;
    else if(!strcmp(a, "--apu")) apuMode = atoi(v), i++;
    else if(!strcmp(a, "--exact")) g_exact = true;
    else {
      usage();
      return 2;
    }
  }
  if(dumpEvery == 0) dumpEvery = 1;

  uint32_t fileSize = 0;
  uint8_t* file = loadFile(path, &fileSize);
  if(!file) {
    fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }
  /* a raw ROM is wrapped in a pack without dictionary */
  NsPack pack;
  uint8_t* packData = NULL;
  uint8_t* rom = NULL;
  NsPackParams params;
  NsRomHeader h; /* params.title points into it */
  memset(&params, 0, sizeof(params));
  if(pack_open(file, fileSize, &pack)) {
    printf("pack: \"%s\", %u dictionary pages\n", pack.title, pack.dictCount);
    params.rom = pack.rom;
    params.romSize = pack.info.romSize;
    params.cartType = pack.info.cartType;
    params.pal = pack.info.pal;
    params.ramSize = pack.info.ramSize;
    params.title = pack.title;
  } else {
    ns_readRomHeader(file, (int)fileSize, &h);
    if(!h.valid) {
      fprintf(stderr, "%s: no SNES header found\n", path);
      return 1;
    }
    static const char* const types[4] = {"?", "LoROM", "HiROM", "ExHiROM"};
    printf("rom: \"%s\" %s %s, %u KB, cart RAM %u KB%s%s\n", h.title, types[h.cartType], h.pal ? "PAL" : "NTSC",
           (fileSize - (uint32_t)h.copierHeader) / 1024, h.ramSize / 1024,
           h.coprocessor ? ", extra chip: " : "", h.coprocessor ? ns_coprocessorName(&h) : "");
    if(h.coprocessor) fprintf(stderr, "warning: extra chips are not emulated, the game will likely not work\n");
    uint32_t romSize = 0;
    rom = ns_padRom(file + h.copierHeader, fileSize - (uint32_t)h.copierHeader, &romSize);
    params.rom = rom;
    params.romSize = romSize;
    params.cartType = h.cartType;
    params.pal = h.pal;
    params.ramSize = h.ramSize > 0x8000 ? 0x8000 : h.ramSize;
    params.title = h.title;
  }
  params.apuMode = apuMode < 0 ? 0xff : (uint8_t)apuMode;
  params.frameSkip = 0xff;
  if(!pack.base || apuMode >= 0) {
    if(!pack.base) {
      uint32_t size = 0;
      packData = prof_buildPack(NULL, &params, &size, NULL);
      if(!packData || !pack_open(packData, size, &pack)) {
        fprintf(stderr, "internal error: cannot build pack\n");
        return 1;
      }
    } else {
      pack.apuMode = (uint8_t)apuMode;
    }
  }

  Inputs in = {seed, NULL, 0};
  if(inputPath && !loadScript(inputPath, &in, frames)) {
    fprintf(stderr, "cannot read %s\n", inputPath);
    return 1;
  }
  const uint32_t unlimited = NS_VP_COUNT;
  uint32_t poolPages = poolKb ? poolKb * 1024 / NS_PAGE_SIZE : unlimited;
  int rc = 0;

  if(profileOut) {
    /* ---- profile: run with plenty of RAM, record stable pages, write a pack */
    Session* se = sessionNew(&pack, unlimited, false);
    NsProfiler* prof = prof_new();
    for(uint32_t f = 0; f < frames; f++) {
      snes_setPad(&se->snes, 0, inputAt(&in, f));
      snes_runFrame(&se->snes);
      prof_sample(prof, &se->snes);
    }
    params.maxPackBytes = budgetKb * 1024;
    uint32_t size = 0;
    NsPackStats st;
    uint8_t* out = prof_buildPack(prof, &params, &size, &st);
    if(!out || !saveFile(profileOut, out, size)) {
      fprintf(stderr, "cannot write %s\n", profileOut);
      return 1;
    }
    printf("profile: %u frames, %u stable page contents (%u dropped)\n", st.framesSampled, st.candidates, st.dropped);
    printf("pack: %u bytes, dictionary %u pages (%u found in the ROM, %u stored, %u left out)\n",
           st.packBytes, st.dictEntries, st.dictInRom, st.dictStored, st.dictLeftOut);
    free(out);
    prof_free(prof);
    sessionFree(se);
  } else if(minPool) {
    /* ---- smallest pool that survives the run */
    uint32_t peak = 0, oomFrame = 0;
    if(!runDevice(&pack, &in, frames, unlimited, &peak, NULL)) {
      printf("min-pool: fails even with unlimited RAM?\n");
      return 1;
    }
    uint32_t lo = 1, hi = unlimited; /* hi always works */
    while(lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      if(runDevice(&pack, &in, frames, mid, NULL, &oomFrame)) hi = mid;
      else lo = mid + 1;
    }
    printf("min-pool: %u pages = %.1f KB (over %u frames, seed %u)\n", hi, hi * 256 / 1024.0, frames, seed);
  } else if(compare) {
    /* ---- reference (accurate renderer, plain RAM) against the device build */
    Session* a = sessionNew(&pack, unlimited, true);
    Session* b = sessionNew(&pack, poolPages, false);
    uint32_t badFrames = 0, firstBad = 0, memBad = 0;
    for(uint32_t f = 0; f < frames; f++) {
      uint16_t pad = inputAt(&in, f);
      snes_setPad(&a->snes, 0, pad);
      snes_setPad(&b->snes, 0, pad);
      snes_runFrame(&a->snes);
      snes_runFrame(&b->snes);
      if(collectEach) pager_collect(&b->snes.pager);
      if(b->snes.pager.oom) {
        printf("compare: device ran out of RAM at frame %u\n", f);
        rc = 1;
        break;
      }
      unsigned where = 0;
      if(!sameMemory(a, b, &where)) {
        if(memBad++ == 0) {
          printf("compare: memory differs at frame %u, %s page %u\n", f, spaceName(where),
                 where < NS_VP_VRAM ? where : (where < NS_VP_SRAM ? where - NS_VP_VRAM : where - NS_VP_SRAM));
        }
      }
      if(memcmp(a->fb, b->fb, sizeof(a->fb)) != 0) {
        if(badFrames++ == 0) {
          firstBad = f;
          int px = 0;
          for(int i = 0; i < NS_SCREEN_W * FB_H; i++) px += a->fb[i] != b->fb[i];
          printf("compare: first different frame %u (%d pixels)\n", f, px);
          if(dumpDir) {
            char name[512];
            snprintf(name, sizeof(name), "%s/diff_%05u_ref.ppm", dumpDir, f);
            savePpm(name, a->fb);
            snprintf(name, sizeof(name), "%s/diff_%05u_dev.ppm", dumpDir, f);
            savePpm(name, b->fb);
          }
        }
      }
      if(dumpDir && (f % dumpEvery) == dumpEvery - 1) {
        char name[512];
        snprintf(name, sizeof(name), "%s/frame_%05u.ppm", dumpDir, f);
        savePpm(name, b->fb);
      }
    }
    pager_collect(&b->snes.pager);
    printf("compare: %u frames, %u with different pixels%s, %u with different memory\n", frames, badFrames,
           badFrames ? " (first shown above)" : "", memBad);
    printf("device: pool %u pages (%.1f KB), peak used %u pages (%.1f KB), %u copy-on-write, %u collections,"
           " given back: %u zero + %u dictionary\n",
           b->snes.pager.poolPages, b->snes.pager.poolPages / 4.0, b->snes.pager.peakUsed,
           b->snes.pager.peakUsed / 4.0, b->snes.pager.statCow, b->snes.pager.statCollects,
           b->snes.pager.statDedupZero, b->snes.pager.statDedupDict);
    (void)firstBad;
    if(badFrames || memBad) rc = 1;
    sessionFree(a);
    sessionFree(b);
  } else {
    /* ---- plain run */
    Session* se = sessionNew(&pack, poolPages, ref);
    uint32_t runHash = 2166136261u;
    for(uint32_t f = 0; f < frames; f++) {
      snes_setPad(&se->snes, 0, inputAt(&in, f));
      snes_runFrame(&se->snes);
      if(collectEach) pager_collect(&se->snes.pager);
      if(se->snes.pager.oom) {
        printf("out of RAM at frame %u\n", f);
        rc = 1;
        break;
      }
      if(hash) runHash = fnv(se->fb, sizeof(se->fb), runHash);
      if(hashEach) printf("f%u %08x\n", f, fnv(se->fb, sizeof(se->fb), 2166136261u));
      if(dumpDir && (f % dumpEvery) == dumpEvery - 1) {
        char name[512];
        snprintf(name, sizeof(name), "%s/frame_%05u.ppm", dumpDir, f);
        savePpm(name, se->fb);
      }
    }
    pager_collect(&se->snes.pager);
    if(hash) printf("hash: last frame %08x, run %08x\n", fnv(se->fb, sizeof(se->fb), 2166136261u), runHash);
    printf("pool %u pages (%.1f KB), peak used %u pages (%.1f KB), %u copy-on-write, %u collections\n",
           se->snes.pager.poolPages, se->snes.pager.poolPages / 4.0, se->snes.pager.peakUsed,
           se->snes.pager.peakUsed / 4.0, se->snes.pager.statCow, se->snes.pager.statCollects);
    sessionFree(se);
  }
  free(in.script);
  free(packData);
  free(rom);
  free(file);
  return rc;
}
