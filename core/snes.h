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
  /* interrupt lines */
  bool irqWanted;
  bool nmiWanted;
  bool resetWanted;
  bool intPend; /* nmiWanted || (irqWanted && !i), kept up to date by whoever changes one of them */
  /* Everything that keeps the next opcode from simply being fetched. The
   * four flags are read as one word by the fast core. */
  union {
    struct {
      bool waiting;   /* WAI */
      bool stopped;   /* STP */
      bool intWanted; /* an interrupt is taken instead of the next opcode */
      bool frameDone; /* the frame is over: leave cpu_run() */
    };
    uint32_t special;
  };
  /* wait-loop detection: state at the last backward jump */
  uint32_t loopTarget;
  uint32_t loopSig[3];
} Cpu;

void cpu_reset(Snes* s, bool hard);
void cpu_run(Snes* s); /* runs until the frame is over */
#ifdef NS_WITH_REFERENCE
void cpu_refRunOpcode(Snes* s); /* LakeSnes' interpreter, one opcode at a time */
#endif

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
/* The picture is drawn late and in bands of this many lines (see ppu.c) */
#define NS_BAND 16
/* how many register writes can wait for the picture to be drawn */
#define NS_WLOG 256
/* at most 34 sprite tiles (8 pixels wide) are fetched for a line */
#define NS_SLIVERS 34

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
  /* ---- renderer (ppu.c) ---- */
  uint8_t renderedLine;  /* last line of this frame already drawn (0: none) */
  uint8_t regLast[0x40]; /* last value written to each register... */
  uint64_t regKnown;     /* ...when its bit is set here */
  /* register writes waiting for the lines before them to be drawn:
   * line << 16 | register << 8 | value */
  uint32_t wlog[NS_WLOG];
  uint16_t wlogCount;
  uint32_t statFastLines; /* statistics: lines drawn by the fast path... */
  uint32_t statSlowLines; /* ...and by the general renderer */
  uint32_t statWhy[8];    /* ...because of: see the WHY_ codes in ppu.c */
  uint8_t why;
  uint8_t lutBrightness; /* brightness the tables below were built for (0xff: none) */
  uint8_t lut5[32];      /* 5-bit channel -> 5-bit output (red, blue) */
  uint8_t lut6[32];      /* 5-bit channel -> 6-bit output (green) */
  uint16_t pal565[0x100]; /* cgram at the current brightness, as RGB565 */
  uint32_t cgramGen;      /* counts the writes to cgram */
  /* cgram blended once and for all with the colour it is always mixed with
   * (see the fast path), and what it was built for */
  uint16_t palBlend[0x100];
  uint32_t palBlendKey[3];
  /* sprites that can show up somewhere on screen, in the order they are
   * scanned: saves looking at all 128 on every line */
  uint8_t objList[128];
  uint8_t objCount;
  uint8_t objListStart; /* first sprite of the scan the list was built for */
  bool objListDirty;
  /* bit plane byte -> 8 pixels, 4 bits each, leftmost pixel in the low
   * bits ([0]) or in the high bits ([1], flipped tiles). In RAM on purpose. */
  uint32_t spread[2][0x100];
  /* line working buffers of the general renderer. One entry per pixel:
   * bits 15-12 priority level, 11-8 layer code, 7-0 cgram index */
  uint16_t zbuf[2][NS_SCREEN_W]; /* main screen, sub screen */
  /* sprite pixels of each line of the band: 0x8000 | priority << 8 | cgram
   * index, or 0. Only the tiles listed in objSliverX can be non-zero. */
  uint16_t objRow[NS_BAND][NS_SCREEN_W];
  int16_t objSliverX[NS_BAND][NS_SLIVERS]; /* left edge of each tile drawn (-7..255) */
  uint8_t objSlivers[NS_BAND];
  uint8_t objPrioMask[NS_BAND]; /* bit p: some sprite pixel of priority p on that line */
  /* RGB565 output of the band being drawn */
  uint16_t band[NS_BAND * NS_SCREEN_W];
} Ppu;

void ppu_reset(Snes* s);
bool ppu_checkOverscan(Snes* s);
void ppu_handleVblank(Snes* s);
void ppu_handleFrameStart(Snes* s);
void ppu_init(Snes* s);  /* once, builds the constant tables */
void ppu_flush(Snes* s); /* draws the lines that are due and not drawn yet */
void ppu_buildLuts(Ppu* ppu);
#ifdef NS_WITH_REFERENCE
void ppu_refRunLine(Snes* s, int line); /* LakeSnes' per-pixel renderer */
#endif
uint8_t ppu_read(Snes* s, uint8_t adr);
void ppu_write(Snes* s, uint8_t adr, uint8_t val);
void ppu_apply(Snes* s, uint8_t adr, uint8_t val); /* the write itself, see ppu_write */

/* --------------------------------------------------------------- cart -- */

enum { NS_CART_NONE = 0, NS_CART_LOROM = 1, NS_CART_HIROM = 2, NS_CART_EXHIROM = 3 };

/* The ROM is cut in blocks of 4 KB. They need not be next to each other in
 * flash, and need not all be there: a pack may hold only the part of a big
 * game that was played on the PC (see pack.h). */
#define NS_ROM_BLOCK_SHIFT 12
#define NS_ROM_BLOCK 4096u
#define NS_NO_BLOCK 0xffffu

typedef struct Cart {
  uint8_t type;
  uint32_t romSize;      /* size of the whole ROM, a power of two */
  uint32_t ramSize;      /* battery RAM, power of two or 0; lives in the pager */
  const uint8_t** block; /* romSize / 4 KB entries: where each block is, or NULL when it is not in the pack */
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

/* Receives the picture: count rows of 256 RGB565 pixels, the first one being
 * row y of the screen (0 = top). Rows come in increasing order, in bands of
 * at most NS_BAND. */
typedef void (*NsPutRows)(void* user, int y, int count, const uint16_t* rgb565);

/* While a DMA is pending, this much is taken off Snes.left so that every
 * bus access of the fast cpu core goes through the slow, complete path. */
#define NS_DMA_BIAS 0x10000000

struct Snes {
  Cpu cpu; /* must stay first: the cpu core casts Cpu* to Snes* */
  /* ---- hot: read on every bus access of the cpu core ---- */
  int32_t left;           /* master cycles until snes_handleEvents must run (<= 0: now) */
  const uint8_t* codePtr; /* codePtr[pc] is the opcode byte, for codeLo <= pc < codeLo + codeLen */
  uint32_t codeLo;
  uint32_t codeLen;       /* 0 when the place the cpu runs from is not plain ROM */
  uint8_t codeCycles;     /* access time of the program bank */
  uint8_t openBus;
  bool loopDirty;         /* the cpu wrote to the bus since the last wait-loop check */
  bool loopIo;            /* ...or read something else than memory */
  uint8_t romCycles[2];   /* access time of ROM in banks 00-7f / 80-ff */
  bool speedHacks;        /* skip loops that only wait; clear for exact timing */
  bool refCpu;            /* host builds only: use the reference interpreter */
  bool romMissing;        /* the game read a part of the ROM that is not in the pack: it is stopped */
  uint32_t romMissingAt;  /* ...at this offset of the ROM */
  uint8_t* romTrace;      /* PC tools: one byte per 256-byte page of the ROM, set when the game reads it
                             (all ROM reads then take the slow path); NULL otherwise */
  int32_t leftBias;       /* NS_DMA_BIAS while a DMA is pending, else 0 */
  /* ---- timing ---- */
  uint16_t nextEvent; /* position in the line (hPos) at which snes_handleEvents must run */
  uint16_t vPos;
  uint16_t lineLen;
  uint16_t irqH; /* hPos of the irq on this line, or 0xffff */
  bool irqLevel; /* v-only irq: condition currently true */
  uint8_t evIdx;
  bool frameReady;
  bool palTiming;
  uint32_t cycleBase; /* master cycles at the start of the line, wraps */
  uint32_t frames;
  uint32_t syncCycle;
  uint32_t autoJoyEnd; /* value of snes_cycles() at which auto-joypad read is over */
  /* video output */
  NsPutRows putRows;
  void* user;
  bool renderFrame; /* false: skip drawing this frame (frame skip) */
  bool refPpu;      /* host builds only: use the slow reference renderer */
  /* input */
  bool padLatchLine;
  uint16_t pad[2]; /* current buttons */
  uint16_t padLatched[2];
  /* wram access through $2180 */
  uint32_t ramAdr;
  /* nmi / irq */
  bool hIrqEnabled;
  bool vIrqEnabled;
  bool nmiEnabled;
  bool inNmi;
  bool inIrq;
  bool inVblank;
  uint16_t hTimer;
  uint16_t vTimer;
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
  Cart cart;
  ApuStub apu;
  Pager pager;
  /* Which ROM block each 32 KB half of each bank starts at (it covers 8 of
   * them), or NS_NO_BLOCK when that half is not plain ROM (system area, work
   * ram, battery ram). */
  uint16_t romHalfBlk[512];
  Dma dma;
  Ppu ppu;
};

/* ROM description, as found in the pack or guessed from a raw ROM image */
typedef struct NsRomInfo {
  uint8_t cartType; /* NS_CART_* */
  bool pal;
  uint32_t romSize; /* power of two */
  uint32_t ramSize; /* power of two or 0 */
} NsRomInfo;

/* The caller provides the RAM pool. Nothing is allocated by the core. */
void snes_init(Snes* s, uint8_t* pool, uint32_t poolBytes, NsPutRows putRows, void* user);
/* The ROM in one piece (rom), or as a table of 4 KB blocks: romSize / 4096
 * little-endian u32, each the offset of a block from base (or from ext when
 * bit 31 is set), or 0xffffffff for a block that is not there. The table of
 * pointers is taken from the pool. Returns false when there is not enough
 * RAM for it. */
bool snes_setRom(Snes* s, const uint8_t* rom, const NsRomInfo* info);
bool snes_setRomBlocks(Snes* s, const uint8_t* base, const uint8_t* ext, const uint8_t* table, const NsRomInfo* info);
void snes_reset(Snes* s, bool hard);
void snes_runFrame(Snes* s);
static inline void snes_setPad(Snes* s, int player, uint16_t buttons) { s->pad[player & 1] = buttons; }

/* used by cpu, dma, ppu */
void snes_handleEvents(Snes* s);
void snes_updateIrq(Snes* s);
void snes_reschedule(Snes* s);
uint8_t snes_readBBus(Snes* s, uint8_t adr);
void snes_writeBBus(Snes* s, uint8_t adr, uint8_t val);
uint8_t snes_read(Snes* s, uint32_t adr);
void snes_write(Snes* s, uint32_t adr, uint8_t val);
void snes_cpuIdle(Snes* s, bool waiting);
uint8_t snes_cpuRead(Snes* s, uint32_t adr);
void snes_cpuWrite(Snes* s, uint32_t adr, uint8_t val);
void snes_syncCycles(Snes* s, bool start, int syncCycles);
void snes_setCodeBank(Snes* s); /* after cpu.k or the memory map changed */
void snes_mapCode(Snes* s, uint32_t pc); /* makes codePtr valid around pc, when that is ROM */
void snes_skipToEvent(Snes* s);
void snes_skipToInterrupt(Snes* s);

/* master cycles since the start of the line */
static inline uint32_t snes_hPos(const Snes* s) {
  return (uint32_t)((int32_t)s->nextEvent - s->left - s->leftBias);
}

/* master cycles since power on, wraps */
static inline uint32_t snes_cycles(const Snes* s) {
  return s->cycleBase + snes_hPos(s);
}

static inline void snes_addCycles(Snes* s, int cycles) {
  s->left -= cycles;
  if(s->left <= 0) snes_handleEvents(s);
}

/* a DMA is waiting (or no longer): see NS_DMA_BIAS */
static inline void snes_dmaBias(Snes* s, bool on) {
  const int32_t bias = on ? NS_DMA_BIAS : 0;
  s->left -= bias - s->leftBias;
  s->leftBias = bias;
}

/* to call after changing cpu.nmiWanted, cpu.irqWanted or cpu.i */
static inline void snes_intPend(Snes* s) {
  s->cpu.intPend = s->cpu.nmiWanted || (s->cpu.irqWanted && !s->cpu.i);
}

/* video ram, 0x8000 words, through the pager */
static inline uint16_t snes_vramRead(const Snes* s, uint32_t wordAdr) {
  const uint8_t* p = s->pager.ptr[NS_VP_VRAM + ((wordAdr & 0x7fff) >> 7)] + ((wordAdr & 0x7f) << 1);
  return (uint16_t)(p[0] | (p[1] << 8));
}

#endif
