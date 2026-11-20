/*
 * nwsnes - system bus, timing and internal registers
 *
 * Register semantics follow LakeSnes (c) 2021-2023 angelo_wf and
 * contributors, MIT license. Timing is rewritten: instead of stepping every
 * 2 master cycles, the CPU runs freely and the few things that happen at
 * fixed positions of a scanline are handled as events.
 */
#include "snes.h"

#include <string.h>

#define IRQ_NONE 0xffffu

/* fixed events of a scanline, by hPos (master cycles) */
enum { EV_HDMA_INIT, EV_RENDER, EV_DRAM_REFRESH, EV_HDMA_RUN, EV_END_OF_LINE };
static const uint16_t kEvPos[4] = {16, 512, 536, 1104};

static void lineStart(Snes* s);
static void mapRom(Snes* s);

static inline uint16_t evPos(const Snes* s) {
  return s->evIdx < EV_END_OF_LINE ? kEvPos[s->evIdx] : s->lineLen;
}

/* Is there anything to do on this line at this event? Most lines only have
 * the DRAM refresh and their own end: the others are stepped over, so the
 * cpu core is interrupted as little as possible. */
static inline bool evNeeded(const Snes* s, unsigned idx) {
  switch(idx) {
    case EV_HDMA_INIT: return s->vPos == 0;
#ifdef NS_WITH_REFERENCE
    /* the reference renderer draws each line halfway through it, like
     * LakeSnes does; the normal one draws late (ppu_flush) and needs no event */
    case EV_RENDER: return s->refPpu && !s->inVblank && s->vPos > 0 && s->renderFrame;
#else
    case EV_RENDER: return false;
#endif
    case EV_HDMA_RUN: return !s->inVblank && s->dma.hdmaAny;
    default: return true;
  }
}

static inline void evSkip(Snes* s) {
  while(s->evIdx < EV_END_OF_LINE && !evNeeded(s, s->evIdx)) s->evIdx++;
}

/* moves the point at which snes_handleEvents runs, keeping the position in the line */
static inline void setNextEvent(Snes* s, uint16_t pos) {
  s->left += (int32_t)pos - (int32_t)s->nextEvent;
  s->nextEvent = pos;
}

void snes_init(Snes* s, uint8_t* pool, uint32_t poolBytes, NsPutRows putRows, void* user) {
  memset(s, 0, sizeof(*s));
  pager_init(&s->pager, pool, poolBytes);
  ppu_init(s);
  s->putRows = putRows;
  s->user = user;
  s->renderFrame = true;
  s->speedHacks = true;
  s->codeMin = 0x10000;
  s->apu.forcedMode = 0xff;
}

void snes_setRom(Snes* s, const uint8_t* rom, const NsRomInfo* info) {
  s->cart.type = info->cartType;
  s->cart.rom = rom;
  s->cart.romSize = info->romSize;
  s->cart.ramSize = info->ramSize > 0x8000 ? 0x8000 : info->ramSize;
  s->palTiming = info->pal;
  mapRom(s);
}

void snes_reset(Snes* s, bool hard) {
  if(hard) pager_reset(&s->pager); /* work ram, video ram and battery ram all read as zero */
  cpu_reset(s, hard);
  apu_reset(s);
  dma_reset(s);
  ppu_reset(s);
  s->padLatchLine = false;
  s->padLatched[0] = s->padLatched[1] = 0;
  s->ramAdr = 0;
  s->vPos = 0;
  s->frames = 0;
  s->cycleBase = 0;
  s->syncCycle = 0;
  s->hIrqEnabled = false;
  s->vIrqEnabled = false;
  s->nmiEnabled = false;
  s->hTimer = 0x1ff;
  s->vTimer = 0x1ff;
  s->inNmi = false;
  s->inIrq = false;
  s->irqLevel = false;
  s->inVblank = false;
  memset(s->portAutoRead, 0, sizeof(s->portAutoRead));
  s->autoJoyRead = false;
  s->autoJoyBusy = false;
  s->ppuLatch = false;
  s->multiplyA = 0xff;
  s->multiplyResult = 0xfe01;
  s->divideA = 0xffff;
  s->divideResult = 0x101;
  s->fastMem = false;
  s->openBus = 0;
  s->loopDirty = true;
  s->loopIo = true;
  s->cpu.loopTarget = 0xffffffffu;
  s->frameReady = false;
  s->cpu.frameDone = false;
  s->evIdx = 0;
  s->irqH = IRQ_NONE;
  s->leftBias = 0;
  lineStart(s);
  evSkip(s);
  s->nextEvent = evPos(s) < s->irqH ? evPos(s) : s->irqH;
  s->left = s->nextEvent; /* position 0 of line 0 */
  mapRom(s);
  snes_intPend(s);
}

void snes_runFrame(Snes* s) {
  s->frameReady = false;
  s->cpu.frameDone = false;
#ifdef NS_WITH_REFERENCE
  if(s->refCpu) {
    while(!s->frameReady) cpu_refRunOpcode(s);
  } else
#endif
  cpu_run(s);
  apu_frame(s);
  pager_frameEnd(&s->pager);
}

/* ------------------------------------------------------------ timing -- */

/* hPos at which the h/v timer irq fires on the current line, or IRQ_NONE.
 * Only called when hIrqEnabled. */
static uint16_t hIrqPos(const Snes* s) {
  if(s->vIrqEnabled && s->vPos != s->vTimer) return IRQ_NONE;
  uint32_t pos = (uint32_t)s->hTimer * 4;
  return pos < s->lineLen ? (uint16_t)pos : IRQ_NONE;
}

/* called when the irq registers change in the middle of a line */
void snes_updateIrq(Snes* s) {
  s->irqH = IRQ_NONE;
  if(s->hIrqEnabled) {
    s->irqLevel = false;
    uint16_t pos = hIrqPos(s);
    if(pos != IRQ_NONE && pos > snes_hPos(s)) s->irqH = pos;
  } else if(s->vIrqEnabled) {
    bool level = s->vPos == s->vTimer;
    if(level && !s->irqLevel) {
      s->inIrq = true;
      s->cpu.irqWanted = true;
      snes_intPend(s);
    }
    s->irqLevel = level;
  } else {
    s->irqLevel = false;
  }
  setNextEvent(s, evPos(s) < s->irqH ? evPos(s) : s->irqH);
}

static void doAutoJoypad(Snes* s) {
  /* the pads are latched then clocked 16 times; first bit out lands in bit 15 */
  for(int p = 0; p < 2; p++) {
    uint16_t state = s->pad[p];
    uint16_t out = 0;
    for(int i = 0; i < 16; i++) {
      if(state & (1 << i)) out |= (uint16_t)(1 << (15 - i));
    }
    s->portAutoRead[p] = out;
    s->padLatched[p] = 0xffff;
  }
  s->portAutoRead[2] = 0;
  s->portAutoRead[3] = 0;
  s->padLatchLine = false;
}

/* things that happen when a line begins (hPos == 0) */
static void lineStart(Snes* s) {
  Ppu* ppu = &s->ppu;
  if(!s->palTiming) {
    s->lineLen = (s->vPos == 240 && !ppu->evenFrame && !ppu->frameInterlace) ? 1360 : 1364;
  } else {
    s->lineLen = (s->vPos == 311 && !ppu->evenFrame && ppu->frameInterlace) ? 1368 : 1364;
  }
  bool startingVblank = false;
  if(s->vPos == 0) {
    s->inVblank = false;
    s->inNmi = false;
    s->ppu.renderedLine = 0;
    ppu_handleFrameStart(s);
  } else if(s->vPos == 225) {
    startingVblank = !ppu_checkOverscan(s);
  } else if(s->vPos == 240) {
    if(!s->inVblank) startingVblank = true;
  }
  if(startingVblank) {
    ppu_flush(s); /* the rest of the picture, before vertical blanking changes anything */
    ppu_handleVblank(s);
    s->inVblank = true;
    s->inNmi = true;
    if(s->autoJoyRead) {
      s->autoJoyBusy = true;
      s->autoJoyEnd = snes_cycles(s) + 4224;
      doAutoJoypad(s);
    }
    if(s->nmiEnabled) s->cpu.nmiWanted = true;
    s->frameReady = true;
    s->cpu.frameDone = true;
  }
  /* h/v timer */
  s->irqH = IRQ_NONE;
  if(s->hIrqEnabled) {
    s->irqH = hIrqPos(s);
  } else if(s->vIrqEnabled) {
    bool level = s->vPos == s->vTimer;
    if(level && !s->irqLevel) {
      s->inIrq = true;
      s->cpu.irqWanted = true;
    }
    s->irqLevel = level;
  }
  snes_intPend(s);
}

/* s->nextEvent stays put while the events are handled, so that snes_hPos()
 * keeps giving the position in the line to whatever runs in here. */
void snes_handleEvents(Snes* s) {
  for(;;) {
    const uint16_t pos = evPos(s);
    const uint32_t hPos = snes_hPos(s);
    if(s->irqH <= pos) {
      if(hPos < s->irqH) break;
      s->irqH = IRQ_NONE;
      s->inIrq = true;
      s->cpu.irqWanted = true;
      snes_intPend(s);
      continue;
    }
    if(hPos < pos) break;
    switch(s->evIdx++) {
      case EV_HDMA_INIT:
        dma_initHdma(s);
        break;
      case EV_RENDER:
#ifdef NS_WITH_REFERENCE
        ppu_refRunLine(s, s->vPos);
#endif
        break;
      case EV_DRAM_REFRESH:
        s->left -= 40;
        break;
      case EV_HDMA_RUN:
        dma_doHdma(s);
        break;
      default: {
        s->left += s->lineLen;
        s->cycleBase += s->lineLen;
        s->vPos++;
        Ppu* ppu = &s->ppu;
        uint16_t lines = s->palTiming ? 312 : 262;
        if(ppu->frameInterlace && ppu->evenFrame) lines++;
        if(s->vPos >= lines) {
          s->vPos = 0;
          s->frames++;
        }
        s->evIdx = EV_HDMA_INIT;
        lineStart(s);
        break;
      }
    }
    evSkip(s);
  }
  const uint16_t pos = evPos(s);
  setNextEvent(s, pos < s->irqH ? pos : s->irqH);
}

/* What there is to do on this line has changed (HDMA switched on or off):
 * pick the next event again from the position in the line. Everything up to
 * that position has already been handled. */
void snes_reschedule(Snes* s) {
  const uint32_t hPos = snes_hPos(s);
  unsigned idx = 0;
  while(idx < EV_END_OF_LINE && kEvPos[idx] <= hPos) idx++;
  s->evIdx = (uint8_t)idx;
  evSkip(s);
  const uint16_t pos = evPos(s);
  setNextEvent(s, pos < s->irqH ? pos : s->irqH);
}

void snes_syncCycles(Snes* s, bool start, int syncCycles) {
  const uint32_t n = (uint32_t)syncCycles;
  const uint32_t cycles = snes_cycles(s);
  if(start) {
    s->syncCycle = cycles;
    snes_addCycles(s, (int)(n - (cycles % n)));
  } else {
    snes_addCycles(s, (int)(n - ((cycles - s->syncCycle) % n)));
  }
}

/* --------------------------------------------------------------- pads -- */

static void padLatch(Snes* s, bool value) {
  s->padLatchLine = value;
  if(value) {
    s->padLatched[0] = s->pad[0];
    s->padLatched[1] = s->pad[1];
  }
}

static uint8_t padRead(Snes* s, int p) {
  if(s->padLatchLine) s->padLatched[p] = s->pad[p];
  uint8_t ret = s->padLatched[p] & 1;
  s->padLatched[p] = (uint16_t)((s->padLatched[p] >> 1) | 0x8000);
  return ret;
}

/* --------------------------------------------------------------- cart -- */

static inline uint8_t sramRead(const Snes* s, uint32_t off) {
  off &= s->cart.ramSize - 1;
  return pager_read(&s->pager, NS_VP_SRAM + (off >> NS_PAGE_SHIFT), off & NS_PAGE_MASK);
}

static inline void sramWrite(Snes* s, uint32_t off, uint8_t val) {
  off &= s->cart.ramSize - 1;
  pager_write(&s->pager, NS_VP_SRAM + (off >> NS_PAGE_SHIFT), off & NS_PAGE_MASK, val);
}

/* offset in battery ram of (bank, adr), or -1 */
static int32_t cartSramOffset(const Cart* c, uint32_t bank, uint32_t adr) {
  if(c->ramSize == 0) return -1;
  switch(c->type) {
    case NS_CART_LOROM:
      if(((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0) && adr < 0x8000) return (int32_t)(((bank & 0xf) << 15) | adr);
      break;
    case NS_CART_HIROM:
    case NS_CART_EXHIROM:
      if((bank & 0x7f) < 0x40 && adr >= 0x6000 && adr < 0x8000) return (int32_t)(((bank & 0x3f) << 13) | (adr & 0x1fff));
      break;
    default:
      break;
  }
  return -1;
}

/* offset in the ROM image of (bank, adr), or -1. Battery ram comes first:
 * only meaningful where cartSramOffset() says no. */
static int32_t cartRomOffset(const Cart* c, uint32_t bank, uint32_t adr) {
  const uint32_t b = bank & 0x7f;
  if(adr < 0x8000 && b < 0x40) return -1;
  switch(c->type) {
    case NS_CART_LOROM:
      return (int32_t)(((b << 15) | (adr & 0x7fff)) & (c->romSize - 1));
    case NS_CART_HIROM:
      return (int32_t)((((b & 0x3f) << 16) | adr) & (c->romSize - 1));
    case NS_CART_EXHIROM:
      return (int32_t)((((b & 0x3f) << 16) | (bank < 0x80 ? 0x400000u : 0u) | adr) & (c->romSize - 1));
    default:
      return -1;
  }
}

static uint8_t cartRead(Snes* s, uint32_t bank, uint32_t adr) {
  const Cart* c = &s->cart;
  int32_t off = cartSramOffset(c, bank, adr);
  if(off >= 0) return sramRead(s, (uint32_t)off);
  off = cartRomOffset(c, bank, adr);
  if(off >= 0) return c->rom[off];
  return s->openBus;
}

static void cartWrite(Snes* s, uint32_t bank, uint32_t adr, uint8_t val) {
  const Cart* c = &s->cart;
  if(c->ramSize == 0) return;
  if(c->type == NS_CART_LOROM) {
    if(((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0) && adr < 0x8000) {
      sramWrite(s, ((bank & 0xf) << 15) | adr, val);
    }
  } else if(c->type == NS_CART_HIROM || c->type == NS_CART_EXHIROM) {
    bank &= 0x7f;
    if(bank < 0x40 && adr >= 0x6000 && adr < 0x8000) {
      sramWrite(s, ((bank & 0x3f) << 13) | (adr & 0x1fff), val);
    }
  }
}

/* ---------------------------------------------------------------- bus -- */

static inline uint8_t wramRead(const Snes* s, uint32_t adr17) {
  return pager_read(&s->pager, NS_VP_WRAM + (adr17 >> NS_PAGE_SHIFT), adr17 & NS_PAGE_MASK);
}

static inline void wramWrite(Snes* s, uint32_t adr17, uint8_t val) {
  pager_write(&s->pager, NS_VP_WRAM + (adr17 >> NS_PAGE_SHIFT), adr17 & NS_PAGE_MASK, val);
}

uint8_t snes_readBBus(Snes* s, uint8_t adr) {
  if(adr < 0x40) return ppu_read(s, adr);
  if(adr < 0x80) return apu_read(s, adr & 0x3);
  if(adr == 0x80) {
    uint8_t ret = wramRead(s, s->ramAdr);
    s->ramAdr = (s->ramAdr + 1) & 0x1ffff;
    return ret;
  }
  return s->openBus;
}

void snes_writeBBus(Snes* s, uint8_t adr, uint8_t val) {
  if(adr < 0x40) {
    ppu_write(s, adr, val);
    return;
  }
  if(adr < 0x80) {
    apu_write(s, adr & 0x3, val);
    return;
  }
  switch(adr) {
    case 0x80:
      wramWrite(s, s->ramAdr, val);
      s->ramAdr = (s->ramAdr + 1) & 0x1ffff;
      break;
    case 0x81:
      s->ramAdr = (s->ramAdr & 0x1ff00) | val;
      break;
    case 0x82:
      s->ramAdr = (s->ramAdr & 0x100ff) | ((uint32_t)val << 8);
      break;
    case 0x83:
      s->ramAdr = (s->ramAdr & 0x0ffff) | ((uint32_t)(val & 1) << 16);
      break;
  }
}

static uint8_t readReg(Snes* s, uint16_t adr) {
  switch(adr) {
    case 0x4210: {
      uint8_t val = 0x2; /* CPU version */
      val |= s->inNmi << 7;
      s->inNmi = false;
      return val | (s->openBus & 0x70);
    }
    case 0x4211: {
      uint8_t val = s->inIrq << 7;
      s->inIrq = false;
      s->cpu.irqWanted = false;
      snes_intPend(s);
      return val | (s->openBus & 0x7f);
    }
    case 0x4212: {
      if(s->autoJoyBusy && (int32_t)(s->autoJoyEnd - snes_cycles(s)) <= 0) s->autoJoyBusy = false;
      uint8_t val = s->autoJoyBusy;
      const uint32_t hPos = snes_hPos(s);
      val |= (hPos < 4 || hPos >= 1096) << 6;
      val |= s->inVblank << 7;
      return val | (s->openBus & 0x3e);
    }
    case 0x4213:
      return s->ppuLatch << 7;
    case 0x4214:
      return s->divideResult & 0xff;
    case 0x4215:
      return s->divideResult >> 8;
    case 0x4216:
      return s->multiplyResult & 0xff;
    case 0x4217:
      return s->multiplyResult >> 8;
    case 0x4218:
    case 0x421a:
    case 0x421c:
    case 0x421e:
      return s->portAutoRead[(adr - 0x4218) / 2] & 0xff;
    case 0x4219:
    case 0x421b:
    case 0x421d:
    case 0x421f:
      return s->portAutoRead[(adr - 0x4219) / 2] >> 8;
    default:
      return s->openBus;
  }
}

static void writeReg(Snes* s, uint16_t adr, uint8_t val) {
  switch(adr) {
    case 0x4200: {
      s->autoJoyRead = val & 0x1;
      if(!s->autoJoyRead) s->autoJoyBusy = false;
      s->hIrqEnabled = val & 0x10;
      s->vIrqEnabled = val & 0x20;
      if(!s->hIrqEnabled && !s->vIrqEnabled) {
        s->inIrq = false;
        s->cpu.irqWanted = false;
      }
      snes_updateIrq(s);
      /* if nmi is enabled while inNmi is still set, immediately generate nmi */
      if(!s->nmiEnabled && (val & 0x80) && s->inNmi) s->cpu.nmiWanted = true;
      s->nmiEnabled = val & 0x80;
      snes_intPend(s);
      break;
    }
    case 0x4201:
      if(!(val & 0x80) && s->ppuLatch) ppu_read(s, 0x37); /* latch the ppu counters */
      s->ppuLatch = val & 0x80;
      break;
    case 0x4202:
      s->multiplyA = val;
      break;
    case 0x4203:
      s->multiplyResult = (uint16_t)(s->multiplyA * val);
      break;
    case 0x4204:
      s->divideA = (s->divideA & 0xff00) | val;
      break;
    case 0x4205:
      s->divideA = (uint16_t)((s->divideA & 0x00ff) | (val << 8));
      break;
    case 0x4206:
      if(val == 0) {
        s->divideResult = 0xffff;
        s->multiplyResult = s->divideA;
      } else {
        s->divideResult = s->divideA / val;
        s->multiplyResult = s->divideA % val;
      }
      break;
    case 0x4207:
      s->hTimer = (s->hTimer & 0x100) | val;
      snes_updateIrq(s);
      break;
    case 0x4208:
      s->hTimer = (uint16_t)((s->hTimer & 0x0ff) | ((val & 1) << 8));
      snes_updateIrq(s);
      break;
    case 0x4209:
      s->vTimer = (s->vTimer & 0x100) | val;
      snes_updateIrq(s);
      break;
    case 0x420a:
      s->vTimer = (uint16_t)((s->vTimer & 0x0ff) | ((val & 1) << 8));
      snes_updateIrq(s);
      break;
    case 0x420b:
      dma_startDma(s, val, false);
      break;
    case 0x420c:
      dma_startDma(s, val, true);
      break;
    case 0x420d:
      s->fastMem = val & 0x1;
      s->romCycles[1] = s->fastMem ? 6 : 8;
      snes_setCodeBank(s); /* the access time of the program bank may have changed */
      break;
    default:
      break;
  }
}

uint8_t snes_read(Snes* s, uint32_t adr) {
  uint32_t bank = adr >> 16;
  uint32_t a = adr & 0xffff;
  uint8_t val;
  if(bank == 0x7e || bank == 0x7f) {
    val = wramRead(s, adr & 0x1ffff);
  } else if(!(bank & 0x40) && a < 0x8000) {
    /* banks 00-3f and 80-bf, system area */
    if(a < 0x2000) {
      val = wramRead(s, a);
    } else if(s->loopIo = true, a >= 0x2100 && a < 0x2200) {
      val = snes_readBBus(s, a & 0xff);
    } else if(a == 0x4016) {
      val = padRead(s, 0) | (s->openBus & 0xfc);
    } else if(a == 0x4017) {
      val = padRead(s, 1) | (s->openBus & 0xe0) | 0x1c;
    } else if(a >= 0x4200 && a < 0x4220) {
      val = readReg(s, (uint16_t)a);
    } else if(a >= 0x4300 && a < 0x4380) {
      val = dma_read(s, (uint16_t)a);
    } else {
      val = cartRead(s, bank, a);
    }
  } else {
    val = cartRead(s, bank, a);
  }
  s->openBus = val;
  return val;
}

void snes_write(Snes* s, uint32_t adr, uint8_t val) {
  s->openBus = val;
  uint32_t bank = adr >> 16;
  uint32_t a = adr & 0xffff;
  if(bank == 0x7e || bank == 0x7f) {
    wramWrite(s, adr & 0x1ffff, val);
    return;
  }
  if(!(bank & 0x40) && a < 0x8000) {
    if(a < 0x2000) {
      wramWrite(s, a, val);
      return;
    }
    if(a >= 0x2100 && a < 0x2200) {
      snes_writeBBus(s, a & 0xff, val);
      return;
    }
    if(a == 0x4016) {
      padLatch(s, val & 1);
      return;
    }
    if(a >= 0x4200 && a < 0x4220) {
      writeReg(s, (uint16_t)a, val);
      return;
    }
    if(a >= 0x4300 && a < 0x4380) {
      dma_write(s, (uint16_t)a, val);
      return;
    }
  }
  cartWrite(s, bank, a, val);
}

static inline int accessTime(const Snes* s, uint32_t adr) {
  uint32_t bank = adr >> 16;
  uint32_t a = adr & 0xffff;
  if(!(bank & 0x40) && a < 0x8000) {
    /* 00-3f,80-bf:0000-7fff */
    if(a < 0x2000 || a >= 0x6000) return 8;
    if(a < 0x4000 || a >= 0x4200) return 6;
    return 12;
  }
  /* 40-7f,c0-ff:0000-ffff and 00-3f,80-bf:8000-ffff */
  return (s->fastMem && bank >= 0x80) ? 6 : 8;
}

/* ----------------------------------------------------------- speed-ups -- */

/* Fills romHalf: where the cpu core may read ROM bytes straight from the
 * image instead of decoding the address. Built from the same two functions
 * cartRead() uses, so both always agree. */
static void mapRom(Snes* s) {
  const Cart* c = &s->cart;
  for(uint32_t h = 0; h < 512; h++) {
    const uint32_t bank = h >> 1;
    const uint32_t adr = (h & 1) << 15;
    const uint8_t* p = NULL;
    if(bank != 0x7e && bank != 0x7f && c->rom && c->romSize >= 0x8000 && cartSramOffset(c, bank, adr) < 0) {
      const int32_t off = cartRomOffset(c, bank, adr);
      if(off >= 0) p = c->rom + off;
    }
    s->romHalf[h] = p;
  }
  s->romCycles[0] = 8;
  s->romCycles[1] = s->fastMem ? 6 : 8;
  snes_setCodeBank(s);
}

/* Opcode bytes are nearly always fetched from ROM: remember where the
 * program bank sits in the image so that a fetch is one load. */
void snes_setCodeBank(Snes* s) {
  const uint32_t k = s->cpu.k;
  const uint8_t* lo = s->romHalf[k * 2];
  const uint8_t* hi = s->romHalf[k * 2 + 1];
  s->codeCycles = s->romCycles[k >> 7];
  if(hi && lo && lo + 0x8000 == hi) {
    s->codePtr = lo;
    s->codeMin = 0;
  } else if(hi) {
    s->codePtr = hi - 0x8000;
    s->codeMin = 0x8000;
  } else {
    s->codePtr = NULL;
    s->codeMin = 0x10000;
  }
}

/* The cpu is going round a loop that changes nothing: only an interrupt or
 * a hardware flag can get it out, and those only change at scanline events
 * (or when horizontal blanking starts). Jump there instead of spinning. */
void snes_skipToEvent(Snes* s) {
  const uint32_t hPos = snes_hPos(s);
  uint32_t target = s->nextEvent;
  if(hPos < 1096 && target > 1096) target = 1096;
  if(target > hPos) snes_addCycles(s, (int)(target - hPos));
}

/* Same, for a loop that does not even look at the hardware: nothing but an
 * interrupt can end it, so run the events until one is taken (or the frame
 * is over). */
void snes_skipToInterrupt(Snes* s) {
  if(s->dma.hdmaAny) { /* HDMA could write to work ram behind the loop's back */
    snes_skipToEvent(s);
    return;
  }
  do {
    const uint32_t hPos = snes_hPos(s);
    if(s->nextEvent > hPos) snes_addCycles(s, (int)(s->nextEvent - hPos));
    else break;
  } while(!s->cpu.intPend && !s->frameReady);
}

__attribute__((noinline)) void snes_cpuIdle(Snes* s, bool waiting) {
  if(s->dma.dmaState) dma_handleDma(s, 6);
  int cycles = 6;
  if(waiting) {
    /* WAI/STP: nothing can change before the next event, jump to it */
    const int left = s->left + s->leftBias;
    if(left > 6) cycles = left;
  }
  snes_addCycles(s, cycles);
}

__attribute__((noinline)) uint8_t snes_cpuRead(Snes* s, uint32_t adr) {
  if(!s->dma.dmaState) {
    /* work ram, by far the most common data access */
    uint32_t ra = 0xffffffffu;
    if((adr >> 17) == 0x3f) ra = adr & 0x1ffff;                        /* 7e0000-7fffff */
    else if(!(adr & 0xe000) && !(adr & 0x400000)) ra = adr & 0x1fff;   /* 0000-1fff of banks 00-3f, 80-bf */
    if(ra != 0xffffffffu) {
      snes_addCycles(s, 8);
      return s->openBus = pager_read(&s->pager, NS_VP_WRAM + (ra >> NS_PAGE_SHIFT), ra & NS_PAGE_MASK);
    }
  }
  int cycles = accessTime(s, adr);
  if(s->dma.dmaState) dma_handleDma(s, cycles);
  snes_addCycles(s, cycles);
  return snes_read(s, adr);
}

__attribute__((noinline)) void snes_cpuWrite(Snes* s, uint32_t adr, uint8_t val) {
  s->loopDirty = true;
  if(!s->dma.dmaState) {
    uint32_t ra = 0xffffffffu;
    if((adr >> 17) == 0x3f) ra = adr & 0x1ffff;
    else if(!(adr & 0xe000) && !(adr & 0x400000)) ra = adr & 0x1fff;
    if(ra != 0xffffffffu) {
      snes_addCycles(s, 8);
      s->openBus = val;
      pager_write(&s->pager, NS_VP_WRAM + (ra >> NS_PAGE_SHIFT), ra & NS_PAGE_MASK, val);
      return;
    }
  }
  int cycles = accessTime(s, adr);
  if(s->dma.dmaState) dma_handleDma(s, cycles);
  snes_addCycles(s, cycles);
  snes_write(s, adr, val);
}
