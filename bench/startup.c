/* nwsnes bench - minimal Cortex-M startup for QEMU's mps2-an500 board */
#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);

void Reset_Handler(void) {
  /* enable the FPU (the device build uses the hard-float ABI) */
  *(volatile uint32_t*)0xE000ED88 |= 0xfu << 20;
  uint32_t* src = &_sidata;
  for(uint32_t* dst = &_sdata; dst < &_edata;) *dst++ = *src++;
  for(uint32_t* dst = &_sbss; dst < &_ebss;) *dst++ = 0;
  main();
  for(;;) {}
}

static void Default_Handler(void) {
  for(;;) {}
}

/* the benchmark's clock: counts wraps of the 24-bit SysTick counter */
volatile uint32_t bench_systick_wraps;
void SysTick_Handler(void) {
  bench_systick_wraps++;
}

__attribute__((section(".vectors"), used))
static void (*const vectors[16])(void) = {
  (void (*)(void))&_estack, Reset_Handler, Default_Handler, Default_Handler,
  Default_Handler, Default_Handler, Default_Handler, 0,
  0, 0, 0, Default_Handler,
  Default_Handler, 0, Default_Handler, SysTick_Handler
};
