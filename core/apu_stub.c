/*
 * nwsnes - stand-in for the audio CPU
 *
 * The calculator has no speaker, and the real audio subsystem (SPC700 + DSP)
 * needs 64 KB of RAM plus a lot of CPU time. It is therefore not emulated.
 * Games still talk to it through four I/O ports ($2140-$2143) and many of
 * them wait for an answer, so this file plays the other side of the usual
 * conversations:
 *
 *  - the boot ROM upload protocol ("IPL"): ports 0/1 read $AA/$BB when the
 *    audio CPU is ready, then every value written to port 0 is acknowledged
 *    by echoing it;
 *  - sound drivers once running: most games only write commands. Those that
 *    wait for an acknowledgement usually wait for an echo, for $AA/$BB (to
 *    upload more data), for a value that changes, or for zero.
 *
 * When a game keeps polling the ports without writing anything, it is stuck
 * waiting, and the stub moves on to the next kind of answer. Per-game
 * profiles can pin a strategy instead (see forcedMode).
 */
#include "snes.h"

enum {
  APU_READY = 0,  /* ports 0/1 read $AA/$BB, 2/3 echo */
  APU_ECHO,       /* every port echoes the last value written to it */
  APU_COUNT,      /* every read returns the next value of a running counter */
  APU_ZERO,       /* every port reads 0 */
  APU_MODES
};

/* polls without a write before the stub tries another answer */
#define STUCK_READS 96

void apu_reset(Snes* s) {
  ApuStub* a = &s->apu;
  uint8_t forced = a->forcedMode;
  a->in[0] = a->in[1] = a->in[2] = a->in[3] = 0;
  a->mode = APU_READY;
  a->counter = 0;
  a->idleReads = 0;
  a->forcedMode = forced;
  if(forced != 0xff) a->mode = forced % APU_MODES;
}

uint8_t apu_read(Snes* s, uint8_t port) {
  ApuStub* a = &s->apu;
  if(a->forcedMode == 0xff && ++a->idleReads >= STUCK_READS) {
    /* the game is waiting for something we are not giving: try the next answer.
     * Waiting for the boot ROM again (to upload more data) is the common case,
     * so "ready" comes right after "echo". */
    static const uint8_t next[APU_MODES] = {APU_COUNT, APU_READY, APU_ZERO, APU_ECHO};
    a->idleReads = 0;
    a->mode = next[a->mode];
  }
  switch(a->mode) {
    case APU_READY:
      if(port == 0) return 0xaa;
      if(port == 1) return 0xbb;
      return a->in[port];
    case APU_ECHO:
      return a->in[port];
    case APU_COUNT:
      return a->counter++;
    default:
      return 0;
  }
}

void apu_write(Snes* s, uint8_t port, uint8_t val) {
  ApuStub* a = &s->apu;
  /* a game that keeps sending the same value while it polls is waiting too
   * (a "reset" command repeated until the boot ROM answers): only a new
   * value counts as progress */
  if(a->in[port] != val) a->idleReads = 0;
  a->in[port] = val;
  if(a->forcedMode != 0xff) return;
  /* $CC on port 0 while the boot ROM is ready starts an upload: from then on
   * every byte is acknowledged by echoing port 0 */
  if(port == 0 && val == 0xcc && a->mode == APU_READY) a->mode = APU_ECHO;
}

void apu_frame(Snes* s) {
  /* a game that polls the ports once per frame from its main loop is not
   * stuck: only count polls that pile up within a frame */
  s->apu.idleReads = 0;
}
