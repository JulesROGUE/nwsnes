/*
 * nwsnes - runs the calculator front end (numworks/main.c) on a PC.
 *
 * The real app talks to the calculator through NumWorks' EADK functions.
 * Here those functions are replaced by stand-ins: a 320x240 picture in
 * memory for the screen, a scripted keyboard, and a clock that only moves
 * when told to. That makes it possible to see the exact screen layout and
 * to exercise the frame pacing code, which otherwise would run for the first
 * time on someone's calculator.
 *
 *   numworks-sim <game.pack|-> <loops> <ms per frame> <out.ppm> [ram KB]
 *
 * "ms per frame" is how long one emulated frame is pretended to take:
 * below 16 the app has time to spare, above it has to skip drawing.
 */
#include <eadk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int app_main(int argc, char* argv[]);

const char* eadk_external_data;
size_t eadk_external_data_size;
uint8_t* ns_sim_heap_start;
uint8_t* ns_sim_heap_end;

static uint16_t screen[EADK_SCREEN_WIDTH * EADK_SCREEN_HEIGHT];
static uint64_t clockMs;
static unsigned loops, maxLoops, costMs;
static const char* outPath;
static unsigned linesPushed, framesDrawn, slept;

static void finish(void) {
  FILE* f = fopen(outPath, "wb");
  if(f) {
    fprintf(f, "P6\n%d %d\n255\n", EADK_SCREEN_WIDTH, EADK_SCREEN_HEIGHT);
    for(int i = 0; i < EADK_SCREEN_WIDTH * EADK_SCREEN_HEIGHT; i++) {
      uint16_t c = screen[i];
      uint8_t rgb[3] = {
        (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)
      };
      fwrite(rgb, 1, 3, f);
    }
    fclose(f);
  }
  printf("sim: %u loops, %u frames drawn (%u lines), %u ms slept, clock at %llu ms\n", loops, framesDrawn, linesPushed,
         slept, (unsigned long long)clockMs);
  exit(0);
}

void eadk_display_push_rect(eadk_rect_t r, const eadk_color_t* pixels) {
  for(int y = 0; y < r.height; y++) {
    for(int x = 0; x < r.width; x++) {
      int sx = r.x + x, sy = r.y + y;
      if(sx < EADK_SCREEN_WIDTH && sy < EADK_SCREEN_HEIGHT) screen[sy * EADK_SCREEN_WIDTH + sx] = pixels[y * r.width + x];
    }
  }
  if(r.width == 256 && r.height == 1) {
    linesPushed++;
    if(r.y == 8) framesDrawn++; /* first line of the game picture */
  }
}

void eadk_display_push_rect_uniform(eadk_rect_t r, eadk_color_t color) {
  for(int y = r.y; y < r.y + r.height && y < EADK_SCREEN_HEIGHT; y++) {
    for(int x = r.x; x < r.x + r.width && x < EADK_SCREEN_WIDTH; x++) screen[y * EADK_SCREEN_WIDTH + x] = color;
  }
}

/* no font here: text is logged, and shown as a bar of the right size */
void eadk_display_draw_string(const char* text, eadk_point_t p, bool large, eadk_color_t fg, eadk_color_t bg) {
  int w = large ? 10 : 7, h = large ? 18 : 14;
  printf("text (%3d,%3d) %s\"%s\"\n", p.x, p.y, large ? "" : "small ", text);
  for(size_t i = 0; text[i]; i++) {
    eadk_rect_t cell = {(uint16_t)(p.x + i * w), p.y, (uint16_t)w, (uint16_t)h};
    eadk_display_push_rect_uniform(cell, bg);
    if(text[i] != ' ') {
      eadk_rect_t ink = {(uint16_t)(cell.x + 1), (uint16_t)(cell.y + h / 3), (uint16_t)(w - 2), (uint16_t)(h / 3)};
      eadk_display_push_rect_uniform(ink, fg);
    }
  }
}

eadk_keyboard_state_t eadk_keyboard_scan(void) {
  /* the app scans once per frame in its main loop */
  loops++;
  if(loops >= maxLoops) finish();
  eadk_keyboard_state_t keys = 0;
  /* the start screen waits for a key press and release */
  static int startKey;
  if(startKey < 6) return (startKey++ >= 2 && startKey <= 5) ? (eadk_keyboard_state_t)1 << eadk_key_ok : 0;
  if(loops >= 5 && loops < 8) keys |= (eadk_keyboard_state_t)1 << eadk_key_zero; /* statistics on */
  if(loops >= 60 && (loops / 40) % 2) keys |= (eadk_keyboard_state_t)1 << eadk_key_right;
  if(loops >= 200) keys |= (eadk_keyboard_state_t)1 << eadk_key_back; /* SNES A */
  return keys;
}

/* The main loop reads the clock twice per frame: before emulating the frame
 * and after. The pretended emulation time is added between the two. */
uint64_t eadk_timing_millis(void) {
  static unsigned calls;
  if(loops > 0 && (calls++ & 1)) clockMs += costMs;
  return clockMs;
}
void eadk_timing_msleep(uint32_t ms) {
  clockMs += ms;
  slept += ms;
}
void eadk_timing_usleep(uint32_t us) { clockMs += us / 1000; }
uint32_t eadk_random(void) { return 4; }

int main(int argc, char** argv) {
  if(argc < 5) {
    fprintf(stderr, "usage: numworks-sim <game.pack|-> <loops> <ms per frame> <out.ppm> [ram KB]\n");
    return 2;
  }
  maxLoops = (unsigned)atoi(argv[2]);
  costMs = (unsigned)atoi(argv[3]);
  outPath = argv[4];
  unsigned ramKb = argc > 5 ? (unsigned)atoi(argv[5]) : 92;
  if(strcmp(argv[1], "-") != 0) {
    FILE* f = fopen(argv[1], "rb");
    if(!f) {
      fprintf(stderr, "cannot read %s\n", argv[1]);
      return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = malloc((size_t)n + 1);
    if(fread(data, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    eadk_external_data = data + 1 - 1;
    eadk_external_data_size = (size_t)n;
  } else {
    eadk_external_data = "none";
    eadk_external_data_size = 4;
  }
  ns_sim_heap_start = malloc(ramKb * 1024);
  ns_sim_heap_end = ns_sim_heap_start + ramKb * 1024;
  app_main(0, NULL);
  finish(); /* the app returned on its own (no game, or out of memory) */
  return 0;
}
