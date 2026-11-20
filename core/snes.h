/*
 * nwsnes - a SNES core sized for the NumWorks calculator
 *
 * CPU, DMA and PPU register handling derive from LakeSnes
 * (c) 2021-2023 angelo_wf and contributors, MIT license.
 * Scheduler, pager, fast renderer and APU stub: nwsnes authors, MIT license.
 */
#ifndef NWSNES_SNES_H
#define NWSNES_SNES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pager.h"

typedef struct Snes Snes;

/* ---------------------------------------------------------------- CPU -- */

typedef struct Cpu {
  /* registers */
  uint16_t a;
  uint16_t x;
  uint16_t y;
  uint16_t sp;
  uint16_t pc;
  uint16_t dp; /* direct page (D) */
  uint8_t k;   /* program bank (PB) */
  uint8_t db;  /* data bank (B) */
  /* flags */
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool d;
  bool xf;
  bool mf;
  bool e;
  /* power state (WAI/STP) */
  bool waiting;
  bool stopped;
  /* interrupts */
  bool irqWanted;
  bool nmiWanted;
  bool intWanted;
  bool resetWanted;
  /* wait-loop detection: state at the last backward jump */
  uint32_t loopTarget;
  uint32_t loopSig[3];
  uint32_t loopWrites;
} Cpu;

void cpu_reset(Snes* s, bool hard);
void cpu_runOpcode(Snes* s);

/* ---------------------------------------------------------------- DMA -- */

typedef struct DmaChannel {
  uint8_t bAdr;
  uint16_t aAdr;
  uint8_t aBank;
  uint16_t size;     /* also indirect hdma adr */
  uint8_t indBank;   /* hdma */
  uint16_t tableAdr; /* hdma */
  uint8_t repCount;  /* hdma */
  uint8_t unusedByte;
  bool dmaActive;
  bool hdmaActive;
  uint8_t mode;
  bool fixed;
  bool decrement;
  bool indirect; /* hdma */
  bool fromB;
  bool unusedBit;
  bool doTransfer; /* hdma */
  bool terminated; /* hdma */
} DmaChannel;

typedef struct Dma {
  DmaChannel channel[8];
  uint8_t dmaState;
  bool hdmaAny; /* at least one channel has hdma enabled */
} Dma;

void dma_reset(Snes* s);
uint8_t dma_read(Snes* s, uint16_t adr);            /* 43x0-43xf */
void dma_write(Snes* s, uint16_t adr, uint8_t val); /* 43x0-43xf */
void dma_handleDma(Snes* s, int cpuCycles);
void dma_startDma(Snes* s, uint8_t val, bool hdma);
void dma_initHdma(Snes* s);
void dma_doHdma(Snes* s);

/* ---------------------------------------------------------------- PPU -- */

typedef struct BgLayer {
  uint16_t hScroll;
  uint16_t vScroll;
  bool tilemapWider;
  bool tilemapHigher;
  uint16_t tilemapAdr;
  uint16_t tileAdr;
  bool bigTiles;
  bool mosaicEnabled;
} BgLayer;

typedef struct Layer {
  bool mainScreenEnabled;
  bool subScreenEnabled;
  bool mainScreenWindowed;
  bool subScreenWindowed;
} Layer;

typedef struct WindowLayer {
  bool window1enabled;
  bool window2enabled;
  bool window1inversed;
  bool window2inversed;
  uint8_t maskLogic;
} WindowLayer;

#define NS_SCREEN_W 256
#define NS_SCREEN_H 224
#define NS_SCREEN_H_OVERSCAN 239

typedef struct Ppu {
  /* vram access (the VRAM itself lives in the pager) */
  uint16_t vramPointer;
  bool vramIncrementOnHigh;
  uint16_t vramIncrement;
  uint8_t vramRemapMode;
  uint16_t vramReadBuffer;
  /* cgram access */
  uint16_t cgram[0x100];
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  /* oam access */
  uint16_t oam[0x100];
  uint8_t highOam[0x20];
  uint8_t oamAdr;
  uint8_t oamAdrWritten;
  bool oamInHigh;
  bool oamInHighWritten;
  bool oamSecondWrite;
  uint8_t oamBuffer;
  /* object/sprites */
  bool objPriority;
  uint16_t objTileAdr1;
  uint16_t objTileAdr2;
  uint8_t objSize;
  bool timeOver;
  bool rangeOver;
  bool objInterlace;
  /* background layers */
  BgLayer bgLayer[4];
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  uint8_t mosaicSize;
  uint8_t mosaicStartLine;
  /* layers */
  Layer layer[5];
  /* mode 7 */
  int16_t m7matrix[8]; /* a, b, c, d, x, y, h, v */
  uint8_t m7prev;
  bool m7largeField;
  bool m7charFill;
  bool m7xFlip;
  bool m7yFlip;
  bool m7extBg;
  /* mode 7 internal */
  int32_t m7startX;
  int32_t m7startY;
  /* windows */
  WindowLayer windowLayer[6];
  uint8_t window1left;
  uint8_t window1right;
  uint8_t window2left;
  uint8_t window2right;
  /* color math */
  uint8_t clipMode;
  uint8_t preventMathMode;
  bool addSubscreen;
  bool subtractColor;
  bool halfColor;
  bool mathEnabled[6];
  uint8_t fixedColorR;
  uint8_t fixedColorG;
  uint8_t fixedColorB;
  /* settings */
  bool forcedBlank;
  uint8_t brightness;
  uint8_t mode;
  bool bg3priority;
  bool evenFrame;
  bool pseudoHires;
  bool overscan;
  bool frameOverscan;
  bool interlace;
  bool frameInterlace;
  bool directColor;
  /* latching */
  uint16_t hCount;
  uint16_t vCount;
  bool hCountSecond;
  bool vCountSecond;
  bool countersLatched;
  uint8_t ppu1openBus;
  uint8_t ppu2openBus;
  /* line renderer working buffers. One entry per pixel:
   * bits 15-12 priority level, 11-8 layer code, 7-0 cgram index */
  uint16_t zbuf[2][NS_SCREEN_W]; /* main screen, sub screen */
  uint16_t objBuf[NS_SCREEN_W];
  bool objBufDirty;
  uint16_t objMinX, objMaxX; /* columns of objBuf in use: [objMinX, objMaxX) */
  uint8_t lutBrightness; /* brightness the tables below were built for (0xff: none) */
  uint8_t lut5[32];      /* 5-bit channel -> 5-bit output (red, blue) */
  uint8_t lut6[32];      /* 5-bit channel -> 6-bit output (green) */
  uint16_t line[NS_SCREEN_W]; /* RGB565 output of the current line */
  uint16_t pal565[0x100];     /* cgram at the current brightness, as RGB565 */
  /* sprites that can show up somewhere on screen, in oam order: saves
   * scanning all 128 on every line */
  uint8_t objList[128];
  uint8_t objCount;
  bool objListDirty;
} Ppu;

void ppu_reset(Snes* s);
bool ppu_checkOverscan(Snes* s);
void ppu_handleVblank(Snes* s);
void ppu_handleFrameStart(Snes* s);
void ppu_runLine(Snes* s, int line); /* line 1..224/239 */
void ppu_buildLuts(Ppu* ppu);
#ifdef NS_WITH_REFERENCE
void ppu_refRunLine(Snes* s, int line); /* LakeSnes' per-pixel renderer */
#endif
uint8_t ppu_read(Snes* s, uint8_t adr);
void ppu_write(Snes* s, uint8_t adr, uint8_t val);

/* --------------------------------------------------------------- cart -- */

enum { NS_CART_NONE = 0, NS_CART_LOROM = 1, NS_CART_HIROM = 2, NS_CART_EXHIROM = 3 };

typedef struct Cart {
  uint8_t type;
  const uint8_t* rom; /* in flash; romSize is a power of two */
  uint32_t romSize;
  uint32_t ramSize;   /* battery RAM, power of two or 0; lives in the pager */
} Cart;

/* ------------------------------------------------- audio CPU stand-in -- */

/* There is no speaker on the calculator and the real audio subsystem needs
 * 64 KB of RAM, so the SPC700 is not emulated. This models just enough of
 * the four I/O ports for games to boot and keep running. */
typedef struct ApuStub {
  uint8_t in[4];      /* last values written by the game */
  uint8_t mode;       /* answer strategy, see apu_stub.c */
  uint8_t counter;
  uint16_t idleReads; /* consecutive port reads without any write */
  uint8_t forcedMode; /* 0xff: automatic */
} ApuStub;

void apu_reset(Snes* s);
uint8_t apu_read(Snes* s, uint8_t port);
void apu_write(Snes* s, uint8_t port, uint8_t val);
void apu_frame(Snes* s);

/* --------------------------------------------------------------- snes -- */

/* joypad bits, in the order the SNES shifts them out (bit 0 first) */
enum {
  NS_BTN_B = 1 << 0,
  NS_BTN_Y = 1 << 1,
  NS_BTN_SELECT = 1 << 2,
  NS_BTN_START = 1 << 3,
  NS_BTN_UP = 1 << 4,
  NS_BTN_DOWN = 1 << 5,
  NS_BTN_LEFT = 1 << 6,
  NS_BTN_RIGHT = 1 << 7,
  NS_BTN_A = 1 << 8,
  NS_BTN_X = 1 << 9,
  NS_BTN_L = 1 << 10,
  NS_BTN_R = 1 << 11
};

typedef void (*NsPutLine)(void* user, int y, const uint16_t* rgb565);

struct Snes {
  Cpu cpu;
  Dma dma;
  Ppu ppu;
  Cart cart;
  ApuStub apu;
  Pager pager;
  bool palTiming;
  /* video output */
  NsPutLine putLine;
  void* user;
  bool renderFrame; /* false: skip drawing this frame (frame skip) */
  bool refPpu;      /* host builds only: use the slow reference renderer */
  /* input */
  uint16_t pad[2];         /* current buttons */
  uint16_t padLatched[2];
  bool padLatchLine;
  /* wram access through $2180 */
  uint32_t ramAdr;
  /* timing */
  uint16_t hPos; /* master cycles since the start of the line */
  uint16_t vPos;
  uint16_t nextEvent; /* hPos at which snes_handleEvents must run */
  uint16_t lineLen;
  uint16_t irqH; /* hPos of the irq on this line, or 0xffff */
  bool irqLevel; /* v-only irq: condition currently true */
  uint8_t evIdx;
  bool frameReady;
  uint32_t frames;
  uint32_t cycles; /* master cycles, wraps */
  uint32_t syncCycle;
  uint32_t autoJoyEnd; /* value of cycles at which auto-joypad read is over */
  /* nmi / irq */
  bool hIrqEnabled;
  bool vIrqEnabled;
  bool nmiEnabled;
  uint16_t hTimer;
  uint16_t vTimer;
  bool inNmi;
  bool inIrq;
  bool inVblank;
  /* joypad handling */
  uint16_t portAutoRead[4];
  bool autoJoyRead;
  bool autoJoyBusy;
  bool ppuLatch;
  /* multiplication/division */
  uint8_t multiplyA;
  uint16_t multiplyResult;
  uint16_t divideA;
  uint16_t divideResult;
  /* misc */
  bool fastMem;
  uint8_t openBus;
  /* speed-ups (see snes.c). speedHacks may be cleared to get the exact,
   * slower behaviour, which the tests compare against. */
  bool speedHacks;
  uint16_t codeBank;      /* bank codePtr is valid for, 0xffff: none */
  uint8_t codeCycles;     /* access time of that bank */
  const uint8_t* codePtr; /* codePtr[adr] for adr 0x8000-0xffff of codeBank */
  uint32_t writeCount;    /* bus writes by the cpu, to spot loops that only wait */
};

/* ROM description, as found in the pack or guessed from a raw ROM image */
typedef struct NsRomInfo {
  uint8_t cartType; /* NS_CART_* */
  bool pal;
  uint32_t romSize; /* power of two */
  uint32_t ramSize; /* power of two or 0 */
} NsRomInfo;

/* The caller provides the RAM pool. Nothing is allocated by the core. */
void snes_init(Snes* s, uint8_t* pool, uint32_t poolBytes, NsPutLine putLine, void* user);
void snes_setRom(Snes* s, const uint8_t* rom, const NsRomInfo* info);
void snes_reset(Snes* s, bool hard);
void snes_runFrame(Snes* s);
static inline void snes_setPad(Snes* s, int player, uint16_t buttons) { s->pad[player & 1] = buttons; }

/* used by cpu, dma, ppu */
void snes_handleEvents(Snes* s);
void snes_updateIrq(Snes* s);
uint8_t snes_readBBus(Snes* s, uint8_t adr);
void snes_writeBBus(Snes* s, uint8_t adr, uint8_t val);
uint8_t snes_read(Snes* s, uint32_t adr);
void snes_write(Snes* s, uint32_t adr, uint8_t val);
void snes_cpuIdle(Snes* s, bool waiting);
uint8_t snes_cpuRead(Snes* s, uint32_t adr);
void snes_cpuWrite(Snes* s, uint32_t adr, uint8_t val);
void snes_syncCycles(Snes* s, bool start, int syncCycles);
void snes_setCodeBank(Snes* s, uint32_t bank);
void snes_skipToEvent(Snes* s);

static inline void snes_addCycles(Snes* s, int cycles) {
  s->hPos = (uint16_t)(s->hPos + cycles);
  s->cycles += (uint32_t)cycles;
  if(s->hPos >= s->nextEvent) snes_handleEvents(s);
}

/* video ram, 0x8000 words, through the pager */
static inline uint16_t snes_vramRead(const Snes* s, uint32_t wordAdr) {
  const uint8_t* p = s->pager.ptr[NS_VP_VRAM + ((wordAdr & 0x7fff) >> 7)] + ((wordAdr & 0x7f) << 1);
  return (uint16_t)(p[0] | (p[1] << 8));
}

#endif
