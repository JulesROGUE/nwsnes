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
assert 'cpu->mem' not in src and 'malloc' not in src and 'sh_handle' not in src
open(sys.argv[2], 'w').write(head + src)
print('ok', len(src.splitlines()), 'lines')
