/*
 * nwsnes - DMA / HDMA
 *
 * Derived from LakeSnes (https://github.com/angelo-wf/LakeSnes)
 * Copyright (c) 2021-2023 angelo_wf and contributors - MIT license.
 * Adapted for nwsnes: HDMA is run straight from the scanline events.
 */
#include "snes.h"

static const uint8_t bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};

static const uint8_t transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};

/* cycles spent inside a scanline event: must not re-enter the event handler */
static inline void rawCycles(Snes* s, int cycles) {
  s->left -= cycles;
}

void dma_reset(Snes* s) {
  Dma* dma = &s->dma;
  for(int i = 0; i < 8; i++) {
    dma->channel[i].bAdr = 0xff;
    dma->channel[i].aAdr = 0xffff;
    dma->channel[i].aBank = 0xff;
    dma->channel[i].size = 0xffff;
    dma->channel[i].indBank = 0xff;
    dma->channel[i].tableAdr = 0xffff;
    dma->channel[i].repCount = 0xff;
    dma->channel[i].unusedByte = 0xff;
    dma->channel[i].dmaActive = false;
    dma->channel[i].hdmaActive = false;
    dma->channel[i].mode = 7;
    dma->channel[i].fixed = true;
    dma->channel[i].decrement = true;
    dma->channel[i].indirect = true;
    dma->channel[i].fromB = true;
    dma->channel[i].unusedBit = true;
    dma->channel[i].doTransfer = false;
    dma->channel[i].terminated = false;
  }
  dma->dmaState = 0;
  dma->hdmaAny = false;
}

uint8_t dma_read(Snes* s, uint16_t adr) {
  const DmaChannel* ch = &s->dma.channel[(adr & 0x70) >> 4];
  switch(adr & 0xf) {
    case 0x0: {
      uint8_t val = ch->mode;
      val |= ch->fixed << 3;
      val |= ch->decrement << 4;
      val |= ch->unusedBit << 5;
      val |= ch->indirect << 6;
      val |= ch->fromB << 7;
      return val;
    }
    case 0x1: return ch->bAdr;
    case 0x2: return ch->aAdr & 0xff;
    case 0x3: return ch->aAdr >> 8;
    case 0x4: return ch->aBank;
    case 0x5: return ch->size & 0xff;
    case 0x6: return ch->size >> 8;
    case 0x7: return ch->indBank;
    case 0x8: return ch->tableAdr & 0xff;
    case 0x9: return ch->tableAdr >> 8;
    case 0xa: return ch->repCount;
    case 0xb:
    case 0xf: return ch->unusedByte;
    default: return s->openBus;
  }
}

void dma_write(Snes* s, uint16_t adr, uint8_t val) {
  DmaChannel* ch = &s->dma.channel[(adr & 0x70) >> 4];
  switch(adr & 0xf) {
    case 0x0:
      ch->mode = val & 0x7;
      ch->fixed = val & 0x8;
      ch->decrement = val & 0x10;
      ch->unusedBit = val & 0x20;
      ch->indirect = val & 0x40;
      ch->fromB = val & 0x80;
      break;
    case 0x1: ch->bAdr = val; break;
    case 0x2: ch->aAdr = (ch->aAdr & 0xff00) | val; break;
    case 0x3: ch->aAdr = (uint16_t)((ch->aAdr & 0xff) | (val << 8)); break;
    case 0x4: ch->aBank = val; break;
    case 0x5: ch->size = (ch->size & 0xff00) | val; break;
    case 0x6: ch->size = (uint16_t)((ch->size & 0xff) | (val << 8)); break;
    case 0x7: ch->indBank = val; break;
    case 0x8: ch->tableAdr = (ch->tableAdr & 0xff00) | val; break;
    case 0x9: ch->tableAdr = (uint16_t)((ch->tableAdr & 0xff) | (val << 8)); break;
    case 0xa: ch->repCount = val; break;
    case 0xb:
    case 0xf: ch->unusedByte = val; break;
    default: break;
  }
}

static void transferByte(Snes* s, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB) {
  /* accessing 0x2180 via b-bus while a-bus accesses ram gives open bus */
  bool validB = !(bAdr == 0x80 && (aBank == 0x7e || aBank == 0x7f || (
    (aBank < 0x40 || (aBank >= 0x80 && aBank < 0xc0)) && aAdr < 0x2000
  )));
  /* accesing b-bus, or dma regs via a-bus gives open bus */
  bool validA = !((aBank < 0x40 || (aBank >= 0x80 && aBank < 0xc0)) && (
    aAdr == 0x420b || aAdr == 0x420c || (aAdr >= 0x4300 && aAdr < 0x4380) || (aAdr >= 0x2100 && aAdr < 0x2200)
  ));
  if(fromB) {
    uint8_t val = validB ? snes_readBBus(s, bAdr) : s->openBus;
    if(validA) snes_write(s, ((uint32_t)aBank << 16) | aAdr, val);
  } else {
    uint8_t val = validA ? snes_read(s, ((uint32_t)aBank << 16) | aAdr) : s->openBus;
    if(validB) snes_writeBBus(s, bAdr, val);
  }
}

/* Where the bytes of an A-bus address can be read directly: work ram or
 * ROM. Returns how many bytes follow at increasing addresses without leaving
 * that page (work ram) or that block (ROM); 0 when the address is
 * something else (registers, battery ram, open bus). */
typedef struct DmaSource {
  const uint8_t* rom; /* ROM: the byte itself */
  uint32_t vpage;     /* work ram: virtual page (the pointer is looked up for every byte, pages can move) */
  uint32_t off;
} DmaSource;

static uint32_t dmaSource(const Snes* s, uint32_t bank, uint32_t adr, DmaSource* src) {
  src->rom = NULL;
  src->vpage = 0;
  src->off = 0;
  if(bank == 0x7e || bank == 0x7f) {
    src->vpage = NS_VP_WRAM + (((bank & 1) << 8) | (adr >> 8));
    src->off = adr & 0xff;
    return 0x100 - src->off;
  }
  if(!(bank & 0x40) && adr < 0x2000) {
    src->vpage = NS_VP_WRAM + (adr >> 8);
    src->off = adr & 0xff;
    return 0x100 - src->off;
  }
  const uint32_t first = s->romHalfBlk[(bank << 1) | (adr >> 15)];
  if(first != NS_NO_BLOCK) {
    const uint8_t* block = s->cart.block[first + ((adr >> NS_ROM_BLOCK_SHIFT) & 7)];
    if(block) {
      src->rom = block + (adr & (NS_ROM_BLOCK - 1));
      return NS_ROM_BLOCK - (adr & (NS_ROM_BLOCK - 1));
    }
  }
  return 0;
}

static void doDma(Snes* s, int cpuCycles) {
  Dma* dma = &s->dma;
  /* align to multiple of 8 */
  snes_syncCycles(s, true, 8);
  /* full transfer overhead */
  snes_addCycles(s, 8);
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(!ch->dmaActive) continue;
    snes_addCycles(s, 8); /* overhead per channel */
    int offIndex = 0;
    while(ch->dmaActive) {
      /* nwsnes: the usual transfer, memory to a B-bus port going up, without
       * decoding both addresses again for every byte. Same accesses in the
       * same order; any byte during which a scanline event is due goes
       * through the general code below. */
      DmaSource src;
      uint32_t run = (ch->fromB || ch->fixed || ch->decrement) ? 0 : dmaSource(s, ch->aBank, ch->aAdr, &src);
      const bool ram = run && !src.rom;
      while(run && s->left > 8) {
        s->left -= 8;
        const uint8_t val = src.rom ? *src.rom++ : s->pager.ptr[src.vpage][src.off++];
        s->openBus = val;
        const uint8_t bAdr = (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode][offIndex++]);
        offIndex &= 3;
        /* 0x2180 via the b-bus while the a-bus reads ram: nothing is written */
        if(bAdr < 0x40) ppu_write(s, bAdr, val);
        else if(!(ram && bAdr == 0x80)) snes_writeBBus(s, bAdr, val);
        ch->aAdr++;
        run--;
        if(--ch->size == 0) {
          ch->dmaActive = false;
          run = 0;
        }
      }
      if(!ch->dmaActive) break;
      snes_addCycles(s, 8); /* hdma may run in here and stop this channel */
      transferByte(s, ch->aAdr, ch->aBank, (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode][offIndex++]), ch->fromB);
      offIndex &= 3;
      if(!ch->fixed) ch->aAdr += ch->decrement ? -1 : 1;
      ch->size--;
      if(ch->size == 0) ch->dmaActive = false;
    }
  }
  /* re-align to cpu cycles */
  snes_syncCycles(s, false, cpuCycles);
}

/* called from the scanline event at the start of the frame */
void dma_initHdma(Snes* s) {
  Dma* dma = &s->dma;
  bool hdmaEnabled = false;
  for(int i = 0; i < 8; i++) {
    if(dma->channel[i].hdmaActive) hdmaEnabled = true;
    dma->channel[i].doTransfer = false;
    dma->channel[i].terminated = false;
  }
  if(!hdmaEnabled) return;
  rawCycles(s, 8); /* full transfer overhead */
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(!ch->hdmaActive) continue;
    ch->dmaActive = false; /* terminate any dma */
    /* load address, repCount, and indirect address if needed */
    rawCycles(s, 8);
    ch->tableAdr = ch->aAdr;
    ch->repCount = snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr++);
    if(ch->repCount == 0) ch->terminated = true;
    if(ch->indirect) {
      rawCycles(s, 8);
      ch->size = snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr++);
      rawCycles(s, 8);
      ch->size |= (uint16_t)(snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr++) << 8);
    }
    ch->doTransfer = true;
  }
}

/* called from the scanline event near the end of every visible line */
void dma_doHdma(Snes* s) {
  Dma* dma = &s->dma;
  if(!dma->hdmaAny) return;
  bool hdmaActive = false;
  int lastActive = 0;
  for(int i = 0; i < 8; i++) {
    if(dma->channel[i].hdmaActive) {
      hdmaActive = true;
      if(!dma->channel[i].terminated) lastActive = i;
    }
  }
  if(!hdmaActive) return;
  rawCycles(s, 8); /* full transfer overhead */
  /* do all copies */
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(ch->hdmaActive) ch->dmaActive = false; /* terminate any dma */
    if(ch->hdmaActive && !ch->terminated && ch->doTransfer) {
      for(int j = 0; j < transferLength[ch->mode]; j++) {
        rawCycles(s, 8);
        if(ch->indirect) {
          transferByte(s, ch->size++, ch->indBank, (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode][j]), ch->fromB);
        } else {
          transferByte(s, ch->tableAdr++, ch->aBank, (uint8_t)(ch->bAdr + bAdrOffsets[ch->mode][j]), ch->fromB);
        }
      }
    }
  }
  /* do all updates */
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(!ch->hdmaActive || ch->terminated) continue;
    ch->repCount--;
    ch->doTransfer = ch->repCount & 0x80;
    rawCycles(s, 8);
    uint8_t newRepCount = snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr);
    if((ch->repCount & 0x7f) == 0) {
      ch->repCount = newRepCount;
      ch->tableAdr++;
      if(ch->indirect) {
        if(ch->repCount == 0 && i == lastActive) {
          /* if this is the last active channel, only fetch high, and use 0 for low */
          ch->size = 0;
        } else {
          rawCycles(s, 8);
          ch->size = snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr++);
        }
        rawCycles(s, 8);
        ch->size |= (uint16_t)(snes_read(s, ((uint32_t)ch->aBank << 16) | ch->tableAdr++) << 8);
      }
      if(ch->repCount == 0) ch->terminated = true;
      ch->doTransfer = true;
    }
  }
}

void dma_handleDma(Snes* s, int cpuCycles) {
  Dma* dma = &s->dma;
  if(dma->dmaState == 1) {
    dma->dmaState = 2;
    return;
  }
  if(dma->dmaState == 2) {
    snes_dmaBias(s, false);
    doDma(s, cpuCycles);
    dma->dmaState = 0;
  }
}

void dma_startDma(Snes* s, uint8_t val, bool hdma) {
  Dma* dma = &s->dma;
  for(int i = 0; i < 8; i++) {
    if(hdma) {
      dma->channel[i].hdmaActive = val & (1 << i);
    } else {
      dma->channel[i].dmaActive = val & (1 << i);
    }
  }
  if(hdma) {
    dma->hdmaAny = val != 0;
    snes_reschedule(s); /* the HDMA event of this line may have to run, or not any more */
  } else {
    dma->dmaState = val != 0 ? 1 : 0;
    snes_dmaBias(s, dma->dmaState != 0); /* the next bus access of the cpu must see it */
  }
}
