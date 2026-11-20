/* nwsnes - order of the code in flash.
 *
 * The calculator's processor has an 8 KB, 2-way instruction cache in front of
 * a flash that takes about a microsecond per missed line. Two lines 4 KB
 * apart compete for the same two cache slots, and a third one 8 KB further
 * evicts one of them. So the code that runs all the time is packed into the
 * first 8 KB (interpreter loop, its helpers, the most used operations, the
 * scanline events), and what follows only disturbs it when it runs.
 * This list is included by the linker scripts of the app and of the bench.
 * See docs/CONCEPTION.md, "Le cache d'instructions". */
*(.text.cpu_run*)
*(.text.cpu_readOpcodeWord*)
*(.text.cpu_fetchSlow*)
*(.text.cpu_idleSlow*)
*(.text.cpu_readWord*)
*(.text.cpu_read*)
*(.text.cpu_writeWord*)
*(.text.cpu_write*)
*(.text.cpu_doBranch*)
*(.text.cpu_adrImp*)
*(.text.cpu_pull*)
*(.text.cpu_push*)
*(.text.cpu_getFlags*)
*(.text.cpu_setFlags*)
*(SORT_BY_NAME(.text.nsop00*))
*(SORT_BY_NAME(.text.nsop01*))
*(.text.snes_handleEvents*)
*(.text.lineStart*)
*(.text.cpu_loopCheck*)
*(.text.snes_skipTo*)
*(.text.snes_cpuRead*)
*(.text.snes_cpuWrite*)
*(.text.snes_cpuIdle*)
*(SORT_BY_NAME(.text.nsop02*))
*(SORT_BY_NAME(.text.nsop03*))
*(SORT_BY_NAME(.text.nsop04*))
*(.text.snes_read*)
*(.text.cartRead*)
*(.text.cartRomOffset*)
*(.text.cartSramOffset*)
*(.text.snes_write*)
*(.text.dma_*)
*(.text.doDma*)
*(.text.transferByte*)
*(.text.ppu_write*)
*(.text.ppu_read*)
*(.text.ppu_getVramRemap*)
*(.text.cpu_special*)
*(.text.cpu_doInterrupt*)
*(.text.snes_setCodeBank*)
*(SORT_BY_NAME(.text.nsop*))
