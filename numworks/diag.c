/*
 * nwsnes-diag - measures what the emulator's speed depends on, on the real
 * calculator: raw speed, the cost of reading the external flash at random,
 * the cost of running code that does not fit the processor's small
 * instruction cache, and whether code may run from RAM at all.
 *
 * It is a separate little app: install it, note the numbers it prints.
 * The last test (code in RAM) is the only risky one: if the system forbids
 * running code from RAM, the calculator restarts. Nothing is damaged, but
 * external apps have to be installed again.
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Diag";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

extern uint8_t _heap_start;
extern uint8_t _heap_end;
extern const uint8_t diag_blob[]; /* 256 KB of noise, in flash */
#define BLOB_MASK 0x3ffffu

static char* putStr(char* dst, const char* src) {
  while(*src) *dst++ = *src++;
  *dst = 0;
  return dst;
}
static char* putNum(char* dst, uint32_t v) {
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while(v);
  while(n) *dst++ = tmp[--n];
  *dst = 0;
  return dst;
}
static int row;
static void line(const char* label, uint32_t a, bool two, uint32_t b) {
  char buf[48];
  char* p = putStr(buf, label);
  p = putNum(p, a);
  if(two) {
    p = putStr(p, " / ");
    p = putNum(p, b);
  }
  eadk_display_draw_string(buf, (eadk_point_t){4, (uint16_t)(4 + row * 18)}, true, eadk_color_white, eadk_color_black);
  row++;
}
static void text(const char* s) {
  eadk_display_draw_string(s, (eadk_point_t){4, (uint16_t)(4 + row * 18)}, true, 0xffe0, eadk_color_black);
  row++;
}
static void waitOk(void) {
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(!eadk_keyboard_key_down(eadk_keyboard_scan(), eadk_key_ok)) eadk_timing_msleep(20);
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
}

/* 1. plain arithmetic in a loop that fits any cache */
static __attribute__((noinline)) uint32_t spin(uint32_t n) {
  uint32_t x = 1;
  for(uint32_t i = 0; i < n; i++) x = x * 1664525u + 1013904223u;
  return x;
}

/* 2-4. byte reads: in order, or at pseudo-random places within mask + 1 bytes */
static __attribute__((noinline)) uint32_t readSeq(const uint8_t* base, uint32_t size, uint32_t passes) {
  uint32_t sum = 0;
  for(uint32_t p = 0; p < passes; p++) {
    for(uint32_t i = 0; i < size; i++) sum += base[i];
  }
  return sum;
}
static __attribute__((noinline)) uint32_t readRandom(const uint8_t* base, uint32_t mask, uint32_t n) {
  uint32_t x = 12345, sum = 0;
  for(uint32_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    sum += base[(x >> 8) & mask];
  }
  return sum;
}

/* 5-6. a function much larger than the instruction cache: 512 small blocks,
 * visited in pseudo-random order (mask 511) or only the first 16 (mask 15) */
#define B1(k) case (k): y = (y ^ (0x9e3779b9u * ((k) + 1u))) * (2u * (k) + 12345u) + (y >> (((k) & 7) + 1)); \
  y ^= y << (((k) & 3) + 2); y += (k) * 77773u + 0x1234567u; break;
#define B4(k) B1(k) B1((k) + 1) B1((k) + 2) B1((k) + 3)
#define B16(k) B4(k) B4((k) + 4) B4((k) + 8) B4((k) + 12)
#define B64(k) B16(k) B16((k) + 16) B16((k) + 32) B16((k) + 48)
#define B256(k) B64(k) B64((k) + 64) B64((k) + 128) B64((k) + 192)
static inline __attribute__((always_inline)) uint32_t walkBody(uint32_t n, uint32_t mask) {
  uint32_t x = 99, y = 7;
  for(uint32_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    switch((x >> 16) & mask) {
      B256(0) B256(256)
      default: break;
    }
  }
  return y;
}
static __attribute__((noinline)) uint32_t walkFlash(uint32_t n, uint32_t mask) { return walkBody(n, mask); }
static __attribute__((noinline, section(".data.ramcode"))) uint32_t walkRam(uint32_t n, uint32_t mask) {
  return walkBody(n, mask);
}

static volatile uint32_t sink;
#define TIME(var, expr) do { uint64_t t0 = eadk_timing_millis(); sink += (expr); var = (uint32_t)(eadk_timing_millis() - t0); } while(0)

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  uint8_t* heap = (uint8_t*)(((uintptr_t)&_heap_start + 3) & ~(uintptr_t)3);
  uint32_t heapBytes = (uint32_t)((uintptr_t)&_heap_end - (uintptr_t)heap);
  uint32_t a, b;

  text("nwsnes diag 1 (temps en ms)");
  line("RAM libre Ko: ", heapBytes / 1024, false, 0);
  TIME(a, spin(20000000));
  line("1 boucle: ", a, false, 0);
  TIME(a, readSeq(diag_blob, BLOB_MASK + 1, 8));
  line("2 flash en ordre: ", a, false, 0);
  TIME(a, readRandom(diag_blob, BLOB_MASK, 300000));
  TIME(b, readRandom(diag_blob, 0x3ff, 300000));
  line("3 flash hasard: ", a, true, b);
  uint32_t ramMask = 0xffff;
  while(ramMask + 1 > heapBytes) ramMask >>= 1;
  memset(heap, 1, ramMask + 1);
  TIME(a, readRandom(heap, ramMask, 300000));
  line("4 RAM hasard: ", a, false, 0);
  TIME(a, walkFlash(300000, 511));
  TIME(b, walkFlash(300000, 15));
  line("5 code flash: ", a, true, b);
  text("Notez ces nombres, puis OK.");
  text("Test 6: si redemarrage,");
  text("le code en RAM est interdit.");
  waitOk();
  TIME(a, walkRam(300000, 511));
  TIME(b, walkRam(300000, 15));
  line("6 code RAM: ", a, true, b);
  text("Fini. Notez la ligne 6.");
  waitOk();
  return 0;
}
