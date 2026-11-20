#!/usr/bin/env python3
"""One-off helper: adapts LakeSnes' cpu.c (MIT) to the nwsnes core.

Usage: import_cpu.py <LakeSnes/snes/cpu.c> <core/cpu.c>
The CPU logic is kept untouched; only the glue changes: no heap, no function
pointers (direct calls into the bus), no save-state handler.
"""
import re, sys
src = open(sys.argv[1]).read()

def cut(text, start_marker, end_marker):
    a = text.index(start_marker); b = text.index(end_marker, a)
    return text[:a] + text[b:]

# header
src = src[src.index('static uint8_t cpu_read(Cpu* cpu, uint32_t adr);'):]
head = '''/*
 * nwsnes - 65816 CPU
 *
 * Derived from LakeSnes (https://github.com/angelo-wf/LakeSnes)
 * Copyright (c) 2021-2023 angelo_wf and contributors - MIT license.
 * Adapted for nwsnes: static state embedded in Snes, direct bus calls.
 */
#include <stdbool.h>
#include <stdint.h>

#include "snes.h"

'''
# drop init/free/handleState
src = cut(src, 'Cpu* cpu_init(', 'void cpu_reset(')
src = cut(src, 'void cpu_handleState(', 'void cpu_runOpcode(')
# public entry points take the Snes
src = src.replace('void cpu_reset(Cpu* cpu, bool hard) {', 'void cpu_reset(Snes* s, bool hard) {\n  Cpu* cpu = &s->cpu;')
src = src.replace('void cpu_runOpcode(Cpu* cpu) {', 'void cpu_runOpcode(Snes* s) {\n  Cpu* cpu = &s->cpu;')
src = cut(src, 'void cpu_nmi(Cpu* cpu) {', 'static uint8_t cpu_read(Cpu* cpu, uint32_t adr) {')
# bus glue: Cpu is the first member of Snes
src = src.replace('return cpu->read(cpu->mem, adr);', 'return snes_cpuRead((Snes*)cpu, adr);')
src = src.replace('cpu->write(cpu->mem, adr, val);', 'snes_cpuWrite((Snes*)cpu, adr, val);')
src = src.replace('cpu->idle(cpu->mem, false);', 'snes_cpuIdle((Snes*)cpu, false);')
src = src.replace('cpu->idle(cpu->mem, true);', 'snes_cpuIdle((Snes*)cpu, true);')
# nwsnes speed-up 1: opcode bytes straight from the ROM image
old = '''static uint8_t cpu_readOpcode(Cpu* cpu) {
  return cpu_read(cpu, (cpu->k << 16) | cpu->pc++);
}'''
new = '''static uint8_t cpu_readOpcode(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  uint32_t pc = cpu->pc++;
  if(pc >= 0x8000 && !s->dma.dmaState) {
    if(cpu->k != s->codeBank) snes_setCodeBank(s, cpu->k);
    if(cpu->k == s->codeBank) {
      snes_addCycles(s, s->codeCycles);
      return s->openBus = s->codePtr[pc];
    }
  }
  return cpu_read(cpu, (cpu->k << 16) | pc);
}

// nwsnes speed-up 2: called when execution goes backwards (a loop). If the
// cpu is at the same place, in the same state, and wrote nothing since last
// time, the loop is only waiting: skip to the next thing that can end it.
static void cpu_loopCheck(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  if(!s->speedHacks) return;
  uint32_t target = ((uint32_t)cpu->k << 16) | cpu->pc;
  uint32_t sig0 = cpu->a | ((uint32_t)cpu->x << 16);
  uint32_t sig1 = cpu->y | ((uint32_t)cpu->sp << 16);
  uint32_t sig2 = cpu->dp | ((uint32_t)cpu->db << 16) | ((uint32_t)cpu_getFlags(cpu) << 24);
  if(target == cpu->loopTarget && s->writeCount == cpu->loopWrites &&
     sig0 == cpu->loopSig[0] && sig1 == cpu->loopSig[1] && sig2 == cpu->loopSig[2]) {
    snes_skipToEvent(s);
  }
  cpu->loopTarget = target;
  cpu->loopWrites = s->writeCount;
  cpu->loopSig[0] = sig0;
  cpu->loopSig[1] = sig1;
  cpu->loopSig[2] = sig2;
}'''
assert old in src
src = src.replace(old, new)
old = '''    cpu_idle(cpu); // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }'''
new = '''    cpu_idle(cpu); // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
    if(value & 0x80) cpu_loopCheck(cpu);
  }'''
assert old in src
src = src.replace(old, new)
# jmp abs going backwards within a page or so: also a loop
old = '''    case 0x4c: { // jmp abs
      cpu->pc = cpu_readOpcodeWord(cpu, true);
      break;
    }'''
new = '''    case 0x4c: { // jmp abs
      uint16_t from = cpu->pc;
      cpu->pc = cpu_readOpcodeWord(cpu, true);
      if((uint16_t)(from - cpu->pc) < 0x100) cpu_loopCheck(cpu);
      break;
    }'''
assert old in src, 'jmp abs'
src = src.replace(old, new)
assert 'cpu->mem' not in src and 'malloc' not in src and 'sh_handle' not in src
open(sys.argv[2], 'w').write(head + src)
print('ok', len(src.splitlines()), 'lines')
