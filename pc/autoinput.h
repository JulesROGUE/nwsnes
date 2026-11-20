/* nwsnes PC side - deterministic "button masher" used by the tests, the
 * benchmark and the automatic part of game profiling. */
#ifndef NWSNES_AUTOINPUT_H
#define NWSNES_AUTOINPUT_H

#include <stdint.h>

#include "../core/snes.h"

static inline uint32_t ns_mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

/* Holds a random set of buttons for a third of a second, and taps Start now
 * and then to get through title screens. Seed 0 means "touch nothing". */
static inline uint16_t ns_autoInput(uint32_t seed, uint32_t frame) {
  if(seed == 0) return 0;
  uint32_t block = frame / 20;
  uint32_t r = ns_mix(seed * 0x9e3779b9u + block);
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

#endif
