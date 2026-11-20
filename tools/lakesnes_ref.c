/*
 * Headless driver for the unmodified LakeSnes emulator, used to cross-check
 * nwsnes frame by frame (see tools/crosscheck.sh). Not part of nwsnes itself.
 *
 *   git clone https://github.com/angelo-wf/LakeSnes
 *   L=LakeSnes/snes
 *   cc -O2 -w -I$L tools/lakesnes_ref.c $L/snes.c $L/snes_other.c $L/cpu.c $L/apu.c \
 *      $L/spc.c $L/dsp.c $L/dma.c $L/ppu.c $L/cart.c $L/input.c $L/statehandler.c -lm -o lakesnes-ref
 *   tools/crosscheck.sh ./lakesnes-ref <directory of .sfc files>
 *
 * usage: lakesnes-ref <rom> [frames] [seed] [last-frame.ppm] [1 = print a hash per frame]
 * It feeds the same pseudo-random buttons as nwsnes-host --seed and converts
 * pixels the same way, so identical emulation gives identical hashes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "snes.h"
#include "ppu.h"
static uint32_t fnv(const void* data, size_t n, uint32_t h) { const uint8_t* p = data; for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static uint16_t autoInput(uint32_t seed, uint32_t frame) {
  if(seed == 0) return 0;
  uint32_t block = frame / 20; uint32_t r = mix(seed * 0x9e3779b9u + block); uint16_t pad = 0;
  if((r & 7) == 0) return (frame % 20) < 4 ? 8 : 0;
  if((r & 7) == 1) return 0;
  switch((r >> 3) & 7) { case 0: pad |= 64; break; case 1: case 2: case 3: pad |= 128; break; case 4: pad |= 16; break; case 5: pad |= 32; break; default: break; }
  if(r & 0x100) pad |= 1; if(r & 0x200) pad |= 2; if((r & 0xc00) == 0xc00) pad |= 256; if((r & 0x3000) == 0x3000) pad |= 512;
  if((r & 0x1c000) == 0x1c000) pad |= 1024; if((r & 0xe0000) == 0xe0000) pad |= 2048;
  if((frame % 20) >= 16) pad &= (uint16_t)~1;
  return pad;
}
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* rom = malloc(n); fread(rom, 1, n, f); fclose(f);
  int frames = argc > 2 ? atoi(argv[2]) : 300; uint32_t seed = argc > 3 ? atoi(argv[3]) : 0;
  const char* dump = argc > 4 ? argv[4] : NULL; int every = argc > 5 ? atoi(argv[5]) : 0;
  Snes* snes = snes_init();
  if(!snes_loadRom(snes, rom, n)) return 1;
  static uint8_t pixels[512 * 4 * 480]; static uint16_t fb[256 * 239];
  uint32_t run = 2166136261u;
  for(int fr = 0; fr < frames; fr++) {
    uint16_t pad = autoInput(seed, fr);
    for(int b = 0; b < 12; b++) snes_setButtonState(snes, 1, b, (pad >> b) & 1);
    snes_runFrame(snes);
    snes_setPixels(snes, pixels);
    memset(fb, 0, sizeof(fb));
    for(int y = 0; y < 224; y++) for(int x = 0; x < 256; x++) {
      const uint8_t* p = snes->ppu->pixelBuffer + (y + (snes->ppu->evenFrame ? 0 : 239)) * 2048 + x * 8 + 4; /* field just drawn, main-screen pixel, BGRX */
      fb[y * 256 + x] = (uint16_t)(((p[3] >> 3) << 11) | ((p[2] >> 2) << 5) | (p[1] >> 3));
    }
    run = fnv(fb, sizeof(fb), run);
    if(every > 0) printf("f%d %08x\n", fr, fnv(fb, sizeof(fb), 2166136261u));
    if(dump && fr == frames - 1) { FILE* o = fopen(dump, "wb"); fprintf(o, "P6\n256 224\n255\n"); for(int i = 0; i < 256 * 224; i++) { uint16_t c = fb[i]; uint8_t rgb[3] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31}; fwrite(rgb, 1, 3, o); } fclose(o); }
  }
  printf("hash: last frame %08x, run %08x\n", fnv(fb, sizeof(fb), 2166136261u), run);
  return 0;
}
