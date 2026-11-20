/*
 * nwsnes-probe - looks at how the calculator's flash is laid out.
 *
 * The N0110 has 8 MB of flash cut in two identical halves ("slots"), each
 * able to hold a complete system: one runs, the other is the spare copy that
 * updates are written to. In each half, what the system does not use is the
 * area for external apps. Apps are normally installed in the area of the
 * running half only, which is what limits the size of a game.
 *
 * This app answers two questions with the real calculator:
 *   1. where exactly is the app area of the running half, and how big;
 *   2. can an app read the other half at all (the system might forbid it),
 *      and how big is the app area there.
 *
 * It only reads. Step 2 is behind a key press: if the system forbids the
 * read, the calculator restarts (nothing is damaged, external apps have to
 * be installed again).
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Sonde";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

#define FLASH_ORIGIN 0x90000000u
#define FLASH_LENGTH 0x00800000u
#define SLOT_LENGTH 0x00400000u
#define SYSTEM_INFO_OFFSET 0x00010000u /* where a half describes its system */
#define SYSTEM_INFO_MAGIC 0xdec0edfeu
#define SECTOR 0x10000u

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
static char* putHex(char* dst, uint32_t v) {
  static const char digits[] = "0123456789ABCDEF";
  for(int i = 7; i >= 0; i--) *dst++ = digits[(v >> (i * 4)) & 15];
  *dst = 0;
  return dst;
}
static int row;
static void text(const char* s, eadk_color_t color) {
  eadk_display_draw_string(s, (eadk_point_t){4, (uint16_t)(4 + row * 18)}, true, color, eadk_color_black);
  row++;
}
static void waitOk(void) {
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(!eadk_keyboard_key_down(eadk_keyboard_scan(), eadk_key_ok)) eadk_timing_msleep(20);
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
}
static uint32_t rd32(uint32_t address) {
  uint32_t v;
  memcpy(&v, (const void*)(uintptr_t)address, 4);
  return v;
}

/* what a half says about itself: system version, app area. Returns false
 * when there is no valid system description there. */
static bool describeHalf(uint32_t slot, uint32_t* appsStart, uint32_t* appsEnd) {
  const uint32_t info = slot + SYSTEM_INFO_OFFSET;
  char buf[48];
  if(rd32(info) != SYSTEM_INFO_MAGIC || rd32(info + 44) != SYSTEM_INFO_MAGIC) {
    text("  pas de systeme valide ici", 0xfbe0);
    return false;
  }
  char* p = putStr(buf, "  systeme ");
  for(int i = 0; i < 8; i++) {
    const char c = *(const char*)(uintptr_t)(info + 4 + (uint32_t)i);
    if(c < 0x20 || c > 0x7e) break;
    *p++ = c;
  }
  *p = 0;
  text(buf, eadk_color_white);
  *appsStart = rd32(info + 20);
  *appsEnd = rd32(info + 24);
  p = putStr(buf, "  applis ");
  p = putHex(p, *appsStart);
  p = putStr(p, "-");
  p = putHex(p, *appsEnd);
  text(buf, eadk_color_white);
  p = putStr(buf, "  = ");
  p = putNum(p, (*appsEnd - *appsStart) / 1024);
  putStr(p, " Ko");
  text(buf, 0x07e0);
  return true;
}

/* how many 64 KB sectors of [start, end) are erased (all 0xFF) */
static uint32_t erasedSectors(uint32_t start, uint32_t end) {
  uint32_t count = 0;
  for(uint32_t s = start; s + SECTOR <= end; s += SECTOR) {
    uint32_t all = 0xffffffffu;
    for(uint32_t a = s; a < s + SECTOR; a += 4) all &= rd32(a);
    if(all == 0xffffffffu) count++;
  }
  return count;
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  char buf[48];
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  const uint32_t here = (uint32_t)(uintptr_t)eadk_app_name;
  const uint32_t slot = FLASH_ORIGIN + ((here - FLASH_ORIGIN) & ~(SLOT_LENGTH - 1));
  const uint32_t other = slot == FLASH_ORIGIN ? FLASH_ORIGIN + SLOT_LENGTH : FLASH_ORIGIN;

  text("nwsnes sonde 1", 0xffe0);
  char* p = putStr(buf, "Appli en ");
  p = putHex(p, here);
  putStr(p, slot == FLASH_ORIGIN ? " (moitie A)" : " (moitie B)");
  text(buf, eadk_color_white);
  text("Moitie en service:", 0xffe0);
  uint32_t a0 = 0, a1 = 0;
  describeHalf(slot, &a0, &a1);
  row++;
  text("OK: lire l'autre moitie.", 0xffe0);
  text("Si la calculatrice redemarre,", eadk_color_white);
  text("c'est que c'est interdit.", eadk_color_white);
  waitOk();

  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  row = 0;
  text("nwsnes sonde 2", 0xffe0);
  /* the read that may not be allowed */
  const uint32_t first = rd32(other + SYSTEM_INFO_OFFSET);
  p = putStr(buf, "Autre moitie lisible: ");
  putHex(p, first);
  text(buf, 0x07e0);
  uint32_t b0 = 0, b1 = 0;
  bool valid = describeHalf(other, &b0, &b1);
  if(!valid || b0 < other || b1 > other + SLOT_LENGTH || b0 >= b1) {
    /* no system there: take the same place as in the running half */
    b0 = a0 - slot + other;
    b1 = a1 - slot + other;
    p = putStr(buf, "  suppose ");
    p = putHex(p, b0);
    p = putStr(p, "-");
    putHex(p, b1);
    text(buf, eadk_color_white);
  }
  const uint32_t from = (b0 + SECTOR - 1) & ~(SECTOR - 1);
  p = putStr(buf, "  secteurs vides: ");
  p = putNum(p, erasedSectors(from, b1));
  p = putStr(p, " sur ");
  putNum(p, (b1 - from) / SECTOR);
  text(buf, eadk_color_white);
  /* the last bytes of the flash, to be sure the whole half answers */
  p = putStr(buf, "  fin de la flash: ");
  putHex(p, rd32(other + SLOT_LENGTH - 4));
  text(buf, eadk_color_white);
  row++;
  text("Notez tout, puis OK.", 0xffe0);
  waitOk();
  return 0;
}
