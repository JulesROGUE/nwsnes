/* QEMU mps2-an500: 4 MB at 0 for code and the game pack, 4 MB of RAM */
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
ENTRY(Reset_Handler)
SECTIONS {
  .text : {
    KEEP(*(.vectors))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : {
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  .bss : {
    _sbss = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
