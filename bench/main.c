/*
 * nwsnes bench - counts the ARM instructions the core executes per frame.
 *
 * Runs the exact code of the calculator app (same compiler, same flags) on
 * QEMU's Cortex-M7 board with instruction counting on, so "one instruction =
 * one nanosecond of virtual time". The game pack is linked into the image.
 *
 * This measures instructions, not seconds: on the real calculator the time
 * per instruction depends on caches and on the external flash, which QEMU
 * does not model. It is still the best available estimate without hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../core/pack.h"
#include "../core/snes.h"
#include "../pc/autoinput.h"

extern const uint8_t bench_pack[];
extern const uint32_t bench_pack_size;

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 600
#endif
#ifndef BENCH_SEED
#define BENCH_SEED 7
#endif
#ifndef BENCH_SKIP
#define BENCH_SKIP 0 /* frames run before the measurement starts (menus, loading) */
#endif
#ifndef BENCH_POOL_KB
#define BENCH_POOL_KB 90
#endif

/* ---- semihosting ---- */
static inline uint32_t semi(uint32_t op, void* arg) {
  register uint32_t r0 __asm__("r0") = op;
  register void* r1 __asm__("r1") = arg;
  __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}
static void out(const char* s) { semi(0x04, (void*)s); }
/* Virtual time, in nanoseconds. Semihosting's own clock follows the host's
 * real time, so the SysTick timer is used instead: QEMU clocks it at 25 MHz
 * of *virtual* time, and with "-icount shift=0" one instruction lasts exactly
 * one virtual nanosecond. Hence: instructions = SysTick counts * 40. */
#define SYST_CSR (*(volatile uint32_t*)0xE000E010)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018)
#define SYSTICK_HZ 25000000u
extern volatile uint32_t bench_systick_wraps;
static void clockStart(void) {
  SYST_RVR = 0xffffff;
  SYST_CVR = 0;
  SYST_CSR = 7; /* enable, interrupt on wrap, processor clock */
  while(SYST_CVR == 0) {} /* the counter only loads its start value on the first tick */
}
static uint64_t ticks(void) {
  uint32_t w, v;
  do {
    w = bench_systick_wraps;
    v = SYST_CVR;
  } while(w != bench_systick_wraps);
  uint64_t counts = ((uint64_t)w << 24) + (0xffffffu - v);
  return counts * (1000000000u / SYSTICK_HZ);
}
static void quit(void) {
  uint32_t args[2] = {0x20026, 0};
  semi(0x20, args);
  for(;;) {}
}
static void outNum(uint64_t v) {
  char buf[24];
  int n = 23;
  buf[n] = 0;
  do {
    buf[--n] = (char)('0' + v % 10);
    v /= 10;
  } while(v);
  out(buf + n);
}

/* inputs: an optional recorded script linked into the image (pairs of
 * little-endian u16: one button state per frame), else the button masher */
extern const uint8_t bench_inputs[];
extern const uint32_t bench_inputs_size;
static uint16_t benchInput(uint32_t f) {
  if(bench_inputs_size >= 2) {
    uint32_t n = bench_inputs_size / 2;
    if(f >= n) f = n - 1;
    return (uint16_t)(bench_inputs[f * 2] | (bench_inputs[f * 2 + 1] << 8));
  }
  return ns_autoInput(BENCH_SEED, f);
}

static Snes snes;
static uint8_t pool[BENCH_POOL_KB * 1024];
static uint16_t sink[NS_BAND * NS_SCREEN_W];
static uint32_t checksum;

static void putRows(void* user, int y, int count, const uint16_t* rows) {
  (void)user;
  memcpy(sink, rows, (size_t)count * NS_SCREEN_W * sizeof(uint16_t)); /* stands for handing the band to the display */
  for(int i = 0; i < count; i++) checksum = checksum * 31 + rows[i * NS_SCREEN_W + (((y + i) * 7) & 255)];
}

/* a store here tells the cache-model plugin (when one is loaded) that a
 * measurement window starts or ends */
volatile uint32_t bench_marker;

#ifndef BENCH_PHASES
#define BENCH_PHASES 7 /* which measurements to run: 1 draw every frame, 2 draw nothing, 4 draw some */
#endif
#ifndef BENCH_DRAW_EVERY
#define BENCH_DRAW_EVERY 4 /* third measurement: draw one frame out of this many, like a slow calculator */
#endif

/* drawEvery: 1 = every frame, 0 = never, n = one frame out of n */
static uint64_t run(const NsPack* pack, uint32_t drawEvery, uint32_t* oomAt) {
  snes_init(&snes, pool, sizeof(pool), putRows, 0);
  if(!pack_attach(pack, &snes)) out("bench: no room for the ROM block table\n");
  snes_reset(&snes, true);
  *oomAt = 0xffffffffu;
  uint64_t t0 = 0;
  for(uint32_t f = 0; f < BENCH_SKIP + BENCH_FRAMES; f++) {
    if(f == BENCH_SKIP) {
      bench_marker = 1;
      t0 = ticks();
    }
    snes_setPad(&snes, 0, benchInput(f));
    snes.renderFrame = f >= BENCH_SKIP && drawEvery && f % drawEvery == 0;
    snes_runFrame(&snes);
    if(snes.pager.oom && *oomAt == 0xffffffffu) *oomAt = f;
  }
  uint64_t t = ticks() - t0;
  bench_marker = 2;
  return t;
}

int main(void) {
  NsPack pack;
  if(!pack_open(bench_pack, bench_pack_size, &pack)) {
    out("bench: bad pack\n");
    quit();
  }
  clockStart();
  uint32_t oom;
  { /* calibration: 1,000,000 iterations of a 3-instruction loop */
    uint64_t t0 = ticks();
    register uint32_t n __asm__("r4") = 1000000;
    __asm__ volatile("1: subs r4, r4, #1\n nop\n bne 1b" : "+r"(n) : : "cc");
    out("calibration: 3000000 instructions measured as "); outNum(ticks() - t0); out("\n");
  }
  out("bench: \""); out(pack.title); out("\", "); outNum(BENCH_FRAMES); out(" frames, pool ");
  outNum(BENCH_POOL_KB); out(" KB\n");
  if(BENCH_PHASES & 1) {
    uint64_t all = run(&pack, 1, &oom);
    out("drawing every frame: "); outNum(all / BENCH_FRAMES); out(" instructions/frame");
    if(oom != 0xffffffffu) { out(" (OUT OF RAM at frame "); outNum(oom); out(")"); }
    pager_collect(&snes.pager); /* the high-water mark is taken when collecting */
    out(", peak RAM pages "); outNum(snes.pager.peakUsed); out(", collections "); outNum(snes.pager.statCollects);
    out(", checksum "); outNum(checksum); out("\n");
  }
  if(BENCH_PHASES & 2) {
    uint64_t none = run(&pack, 0, &oom);
    out("drawing nothing:     "); outNum(none / BENCH_FRAMES); out(" instructions/frame\n");
  }
  if(BENCH_PHASES & 4) {
    uint64_t some = run(&pack, BENCH_DRAW_EVERY, &oom);
    out("drawing 1 frame in "); outNum(BENCH_DRAW_EVERY); out(": "); outNum(some / BENCH_FRAMES); out(" instructions/frame\n");
  }
  quit();
  return 0;
}
