/*
 * nwsnes - NumWorks front end (external app, EADK API)
 *
 * The game comes as the app's external data: a "pack" made on a PC by
 * snes2nw (ROM + page dictionary). All the RAM the calculator leaves to
 * external apps, minus this app's own data (about 30 KB, mostly the buffers
 * of the renderer), becomes the page pool.
 *
 * Home and On/Off are handled by the calculator itself and leave the app.
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../core/pack.h"
#include "../core/snes.h"

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "SNES";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

/* limits of the RAM left after this app's own data, set by the nwlink linker script */
#ifdef NS_SIM
/* PC simulation of this file (host/numworks_sim.c): the "heap" is a plain buffer */
extern uint8_t* ns_sim_heap_start;
extern uint8_t* ns_sim_heap_end;
#define HEAP_START ns_sim_heap_start
#define HEAP_END ns_sim_heap_end
#define RAM_START (ns_sim_heap_start - sizeof(Snes))
#else
extern uint8_t _heap_start;
extern uint8_t _heap_end;
extern uint8_t _data_section_start_ram; /* first byte of the RAM given to the app */
#define RAM_START (&_data_section_start_ram)
#define HEAP_START (&_heap_start)
#define HEAP_END (&_heap_end)
#endif

#define SCREEN_X ((EADK_SCREEN_WIDTH - NS_SCREEN_W) / 2)
#define SCREEN_Y ((EADK_SCREEN_HEIGHT - NS_SCREEN_H) / 2)
#define MAX_SKIP 3 /* frames in a row that may go undrawn to keep up */

static Snes snes;

/* a band of the picture, straight to the display: one call for up to NS_BAND lines */
static void putRows(void* user, int y, int count, const uint16_t* rows) {
  (void)user;
  if(y < 0 || y >= NS_SCREEN_H) return;
  if(y + count > NS_SCREEN_H) count = NS_SCREEN_H - y; /* overscan lines do not fit on the screen */
  eadk_display_push_rect((eadk_rect_t){SCREEN_X, (uint16_t)(SCREEN_Y + y), NS_SCREEN_W, (uint16_t)count},
                         (const eadk_color_t*)rows);
}

/* ------------------------------------------------------------- text -- */

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

static void text(int line, const char* str) {
  eadk_display_draw_string(str, (eadk_point_t){8, (uint16_t)(8 + line * 20)}, true, eadk_color_white, eadk_color_black);
}

static void waitKey(void) {
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(!eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
}

/* ------------------------------------------------------------- input -- */

/*   OK = X    Back = A        the four keys right of the arrows form the
 *   Toolbox = Y  Backspace = B   same square as the pad's face buttons */
static uint16_t readPad(eadk_keyboard_state_t k) {
  uint16_t pad = 0;
  if(eadk_keyboard_key_down(k, eadk_key_up)) pad |= NS_BTN_UP;
  if(eadk_keyboard_key_down(k, eadk_key_down)) pad |= NS_BTN_DOWN;
  if(eadk_keyboard_key_down(k, eadk_key_left)) pad |= NS_BTN_LEFT;
  if(eadk_keyboard_key_down(k, eadk_key_right)) pad |= NS_BTN_RIGHT;
  if(eadk_keyboard_key_down(k, eadk_key_ok)) pad |= NS_BTN_X;
  if(eadk_keyboard_key_down(k, eadk_key_back)) pad |= NS_BTN_A;
  if(eadk_keyboard_key_down(k, eadk_key_toolbox)) pad |= NS_BTN_Y;
  if(eadk_keyboard_key_down(k, eadk_key_backspace)) pad |= NS_BTN_B;
  if(eadk_keyboard_key_down(k, eadk_key_shift)) pad |= NS_BTN_SELECT;
  if(eadk_keyboard_key_down(k, eadk_key_alpha)) pad |= NS_BTN_START;
  if(eadk_keyboard_key_down(k, eadk_key_exe)) pad |= NS_BTN_START;
  if(eadk_keyboard_key_down(k, eadk_key_exp)) pad |= NS_BTN_L;
  if(eadk_keyboard_key_down(k, eadk_key_power)) pad |= NS_BTN_R;
  return pad;
}

/* ------------------------------------------------------------- stats -- */

/* Left margin (32 pixels = four small characters), toggled with the 0 key:
 * frames drawn per second, emulation speed in percent, RAM used by the game
 * and RAM still free, both in KB. */
static void statLine(int row, uint32_t value, char unit) {
  char buf[12];
  char* end = putNum(buf, value);
  if(unit) *end++ = unit;
  *end = 0;
  /* exactly four characters, or the text would spill over the picture */
  char out[5] = {' ', ' ', ' ', ' ', 0};
  int n = (int)(end - buf);
  if(n > 4) n = 4;
  memcpy(out + 4 - n, buf, (size_t)n);
  eadk_display_draw_string(out, (eadk_point_t){2, (uint16_t)(40 + row * 16)}, false, 0xffe0, eadk_color_black);
}

static void drawStats(uint32_t fps, uint32_t speed) {
  statLine(0, fps, 0);
  statLine(1, speed, '%');
  statLine(2, pager_usedPages(&snes.pager) / 4, 'K');
  statLine(3, snes.pager.freeCount / 4u, 'K');
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  char buf[48];
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);

  uint8_t* pool = (uint8_t*)(((uintptr_t)HEAP_START + 3) & ~(uintptr_t)3);
  uint32_t poolBytes = (uint32_t)((uintptr_t)HEAP_END - (uintptr_t)pool);

  NsPack pack;
  if(!pack_open((const uint8_t*)eadk_external_data, eadk_external_data_size, &pack)) {
    text(0, "nwsnes");
    text(2, "Aucun jeu installe.");
    text(3, "Utilisez snes2nw sur PC");
    text(4, "pour installer un jeu.");
    char* p = putStr(buf, "RAM libre: ");
    p = putNum(p, poolBytes / 1024);
    putStr(p, " Ko");
    text(6, buf);
    waitKey();
    return 0;
  }

  snes_init(&snes, pool, poolBytes, putRows, NULL);
  pack_attach(&pack, &snes);
  snes_reset(&snes, true);

  /* start screen. It stays up until a key is pressed so the RAM figure can
   * be read: it is the number snes2nw needs (--ram-kb) to give a verdict. */
  text(0, "nwsnes 0.2");
  text(1, pack.title);
  char* p = putStr(buf, "RAM totale: ");
  p = putNum(p, (uint32_t)((uintptr_t)HEAP_END - (uintptr_t)RAM_START) / 1024);
  putStr(p, " Ko");
  text(3, buf);
  p = putStr(buf, "RAM pour le jeu: ");
  p = putNum(p, snes.pager.poolPages / 4);
  putStr(p, " Ko");
  text(4, buf);
  p = putStr(buf, "Dictionnaire: ");
  p = putNum(p, pack.dictCount);
  putStr(p, " pages");
  text(5, buf);
  text(7, "0: stats   Maison: quitter");
  text(9, "Appuyez sur OK");
  waitKey();
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);

  const uint32_t frameUs = pack.info.pal ? 20000 : 16667;
  uint64_t start = eadk_timing_millis();
  uint32_t frame = 0;     /* frames emulated since start */
  uint32_t skipped = 0;   /* consecutive undrawn frames */
  bool stats = false, statsKey = false;
  uint64_t statsAt = start;
  uint32_t statsFrames = 0, statsDrawn = 0;

  for(;;) {
    eadk_keyboard_state_t keys = eadk_keyboard_scan();
    snes_setPad(&snes, 0, readPad(keys));
    bool zero = eadk_keyboard_key_down(keys, eadk_key_zero);
    if(zero && !statsKey) {
      stats = !stats;
      if(!stats) eadk_display_push_rect_uniform((eadk_rect_t){0, 0, SCREEN_X, EADK_SCREEN_HEIGHT}, eadk_color_black);
    }
    statsKey = zero;

    /* how late are we on real time? */
    uint64_t now = eadk_timing_millis();
    int64_t late = (int64_t)(now - start) - (int64_t)((uint64_t)frame * frameUs / 1000);
    bool render = true;
    if(pack.frameSkip != 0xff) {
      render = frame % ((uint32_t)pack.frameSkip + 1) == 0;
    } else if(late > 8 && skipped < MAX_SKIP) {
      render = false;
    }
    skipped = render ? 0 : skipped + 1;
    if(late > 200) start += (uint64_t)late; /* cannot keep up: slow down rather than skip forever */

    snes.renderFrame = render;
    snes_runFrame(&snes);
    frame++;
    statsFrames++;
    statsDrawn += render;

    if(snes.pager.oom) {
      eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
      text(0, "Memoire pleine");
      text(2, "Ce passage du jeu n'est pas");
      text(3, "dans le dictionnaire. Rejouez");
      text(4, "le sur PC avec snes2nw puis");
      text(5, "reinstallez.");
      waitKey();
      return 0;
    }

    now = eadk_timing_millis();
    if(stats && now - statsAt >= 1000) {
      uint32_t ms = (uint32_t)(now - statsAt);
      drawStats(statsDrawn * 1000 / ms, (uint32_t)((uint64_t)statsFrames * frameUs / 10 / ms));
      statsAt = now;
      statsFrames = statsDrawn = 0;
    } else if(!stats) {
      statsAt = now;
      statsFrames = statsDrawn = 0;
    }
    /* ahead of real time: wait for the frame's slot */
    int64_t early = (int64_t)((uint64_t)frame * frameUs / 1000) - (int64_t)(now - start);
    if(early > 0) eadk_timing_msleep((uint32_t)early);
  }
}
