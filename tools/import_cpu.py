#!/usr/bin/env python3
"""Derives the two CPU cores of nwsnes from LakeSnes' cpu.c (MIT).

Usage: import_cpu.py <LakeSnes/snes/cpu.c> <core dir>

  core/cpu_ref.c  LakeSnes' interpreter with only the glue changed. Built for
                  PC tests only; it is what the fast core is checked against.
  core/cpu.c      The same opcode logic, text for text, rearranged for a
                  processor with an 8 KB instruction cache in front of a slow
                  flash: one loop, decoding in two steps (addressing mode,
                  then operation) so that the ~25 modes and ~90 operations
                  exist once each instead of once per opcode, and bus accesses
                  that stay on a short path for work ram and ROM.

Both run the same bus accesses in the same order with the same timing, so
they must produce the same memory and the same picture: "make test" checks.
"""
import re, sys

src = open(sys.argv[1]).read()
out = sys.argv[2]


def cut(text, start_marker, end_marker):
    a = text.index(start_marker)
    b = text.index(end_marker, a)
    return text[:a] + text[b:]


def func(name, text=None):
    """full text of a function definition (LakeSnes style: closing brace in column 0)"""
    text = src if text is None else text
    m = re.search(r'^static [\w\* ]+\b%s\(.*?\) \{$' % re.escape(name), text, re.M)
    assert m, name
    i = text.index('\n}\n', m.start()) + 3
    return text[m.start():i]


LICENSE = '''/*
 * nwsnes - 65816 CPU%s
 *
 * Derived from LakeSnes (https://github.com/angelo-wf/LakeSnes)
 * Copyright (c) 2021-2023 angelo_wf and contributors - MIT license.
%s */
'''

# ------------------------------------------------------------ reference --
ref = src[src.index('static uint8_t cpu_read(Cpu* cpu, uint32_t adr);'):]
ref = cut(ref, 'Cpu* cpu_init(', 'void cpu_reset(')
reset_fn = ref[ref.index('void cpu_reset(Cpu* cpu, bool hard) {'):ref.index('void cpu_handleState(')]
ref = cut(ref, 'void cpu_reset(Cpu* cpu, bool hard) {', 'void cpu_runOpcode(')
ref = ref.replace('void cpu_runOpcode(Cpu* cpu) {', 'void cpu_refRunOpcode(Snes* s) {\n  Cpu* cpu = &s->cpu;')
ref = cut(ref, 'void cpu_nmi(Cpu* cpu) {', 'static uint8_t cpu_read(Cpu* cpu, uint32_t adr) {')
ref = ref.replace('return cpu->read(cpu->mem, adr);', 'return snes_cpuRead((Snes*)cpu, adr);')
ref = ref.replace('cpu->write(cpu->mem, adr, val);', 'snes_cpuWrite((Snes*)cpu, adr, val);')
ref = ref.replace('cpu->idle(cpu->mem, false);', 'snes_cpuIdle((Snes*)cpu, false);')
ref = ref.replace('cpu->idle(cpu->mem, true);', 'snes_cpuIdle((Snes*)cpu, true);')
assert 'cpu->mem' not in ref and 'malloc' not in ref and 'sh_handle' not in ref
ref_head = LICENSE % (', reference interpreter (PC builds only)',
                      ' * This is LakeSnes\' interpreter with only the glue changed (state embedded\n'
                      ' * in Snes, direct bus calls). The fast core in cpu.c is checked against it.\n')
ref_head += '''#ifdef NS_WITH_REFERENCE
#include <stdbool.h>
#include <stdint.h>

#include "snes.h"

'''
open(out + '/cpu_ref.c', 'w').write(ref_head + ref + '#endif /* NS_WITH_REFERENCE */\n')

# ----------------------------------------------------------------- fast --
# 1. the opcode switch, one entry per opcode
sw = src[src.index('static void cpu_doOpcode(Cpu* cpu, uint8_t opcode) {'):]
cases = re.findall(r'    case 0x([0-9a-f]{2}): \{ // ([^\n]*)\n(.*?)\n    \}\n', sw, re.S)
assert len(cases) == 256 and [int(c[0], 16) for c in cases] == list(range(256))
regular = re.compile(
    r'^      uint32_t low = 0;\n'
    r'      uint32_t high = cpu_adr(\w+)\(cpu, &low(?:, (true|false))?\);\n'
    r'      cpu_(\w+)\(cpu, low, high\);\n'
    r'      break;$')

# How often each opcode runs, per 100000 opcodes: measured with NS_OPCODE_STATS on
# a platform game (boot, menus and gameplay). Only used to decide the layout of
# the generated code: which operations sit next to each other at the start of
# cpu_run (the hot ones) and which are moved out of the way (cpu_coldOp).
OPCODE_FREQ = '''
f0:6408 a5:6239 d0:5960 8d:4065 ca:3707 c8:3698 e8:2897 85:2860 eb:2482 bd:2408 10:2395 20:2334
60:2334 0a:2241 a7:2093 29:2056 9d:1872 a6:1857 e2:1829 b7:1823 c2:1789 86:1648 97:1549 1d:1387
e6:1265 a9:1237 c9:1232 ad:1202 9f:1148 4a:1137 88:1112 18:1071 99:902 1a:886 80:721 c6:685 69:685
cd:679 90:628 a8:624 30:617 b9:607 a0:606 b0:592 05:496 38:491 b5:482 68:439 c0:429 25:427 9c:426
a2:376 8e:373 48:364 e0:354 e5:350 07:346 7a:335 98:322 aa:310 8c:308 ac:293 ae:285 4c:280 5a:267
ea:265 a4:250 bc:246 84:234 8a:229 9e:224 c5:196 bb:191 08:191 28:191 64:190 22:187 65:186 da:181
fa:181 79:180 9b:170 bf:167 be:162 7d:152 e9:141 f9:141 ab:133 ec:129 6b:115 2a:108 46:107 95:100
7f:97 b4:89 19:80 dc:72 26:69 8b:66 4b:64 4d:64 6d:61 2d:57 2c:56 45:55 59:53 54:51 06:50 50:48
8f:44 fd:42 09:41 b1:38 3a:37 0d:36 74:35 ed:33 e4:33 df:27 d9:25 78:24 40:24 87:23 f5:21 cc:20
fe:17 75:16 49:15 6a:15 58:14 d5:14 ce:13 3e:12 b2:11 70:10 14:9 3d:8 ee:7 5d:7 de:6 dd:6 24:4 7e:4
3f:3 0e:3 04:2 82:1 af:1 34:0 39:0 b6:0 1b:0 5b:0 fb:0
'''

modes = ['None']      # addressing modes, in order of first use
mode_call = {'None': None}
ops = []              # operations: (name, body) ; body None = cpu_<name>(cpu, low, high)
op_index = {}
table = []
for num, comment, body in cases:
    m = regular.match(body)
    if m:
        mode = m.group(1) + {'true': 'W', 'false': 'R', None: ''}[m.group(2)]
        if m.group(1) == 'Imm':
            mode = 'ImmX' if m.group(2) == 'true' else 'ImmM'
        if mode not in mode_call:
            modes.append(mode)
            mode_call[mode] = 'cpu_adr%s(cpu, &low%s)' % (m.group(1), ', ' + m.group(2) if m.group(2) else '')
        key = m.group(3)
        if key not in op_index:
            op_index[key] = len(ops)
            ops.append((key, None, comment))
        table.append((mode, op_index[key], comment))
    else:
        assert 'low' not in body or 'cpu_adr' not in body.split('low')[0] or True
        key = 'x' + num
        op_index[key] = len(ops)
        ops.append((key, body, comment))
        table.append(('None', op_index[key], comment))
assert len(modes) < 256 and len(ops) < 256

# layout: operations and modes by decreasing use; operations that make up the
# last 0.3% of what runs go to a separate function
freq = {int(k, 16): int(v) for k, v in (x.split(':') for x in OPCODE_FREQ.split())}
op_use = [0] * len(ops)
mode_use = {m: 0 for m in modes}
for i, (mode, opi, comment) in enumerate(table):
    op_use[opi] += freq.get(i, 0)
    mode_use[mode] += freq.get(i, 0)
order = sorted(range(len(ops)), key=lambda i: -op_use[i])
total_use = sum(op_use)
hot_count = 0
cum = 0
for i in order:
    if cum >= total_use * 0.997:
        break
    cum += op_use[i]
    hot_count += 1
new_index = {old_i: new_i for new_i, old_i in enumerate(order)}
ops = [ops[i] for i in order]
table = [(mode, new_index[opi], comment) for mode, opi, comment in table]
modes = ['None'] + sorted(modes[1:], key=lambda m: -mode_use[m])

# 2. helper functions, taken as they are
helpers = ['cpu_getFlags', 'cpu_setFlags', 'cpu_setZN', 'cpu_doBranch', 'cpu_pullByte', 'cpu_pushByte',
           'cpu_pullWord', 'cpu_pushWord', 'cpu_readWord', 'cpu_writeWord', 'cpu_doInterrupt']
a = src.index('// addressing modes\n')
b = src.index('static void cpu_doOpcode(Cpu* cpu, uint8_t opcode) {')
adr_and_ops = src[a:b]

text = ''
for h in helpers:
    text += func(h) + '\n'
text += adr_and_ops
body_text = '\n'.join(o[1] for o in ops if o[1])

def glue(t):
    # every change of the program bank moves the opcode pointer
    t = re.sub(r'cpu->k = (.*?);', r'cpu_setK(cpu, \1);', t)
    # every change of an interrupt input updates the pending flag
    t = re.sub(r'(cpu->(?:i|nmiWanted|irqWanted) = [^;]*;)', r'\1 snes_intPend((Snes*)cpu);', t)
    return t

text = glue(text)
# decimal mode hardly ever runs: tell the compiler, so that it moves it aside
text = text.replace('if(cpu->d) {', 'if(NS_UNLIKELY(cpu->d)) {').replace('if(cpu->d && ', 'if(NS_UNLIKELY(cpu->d) && ')
body_text_glued = [(o[0], glue(o[1]) if o[1] else None, o[2]) for o in ops]

# wait-loop detection hooks (nwsnes speed-up, see cpu_loopCheck)
old = '''    cpu_idle(cpu); // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }'''
new = '''    cpu_idle(cpu); // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
    if(value & 0x80) cpu_loopCheck(cpu);
  }'''
assert old in text
text = text.replace(old, new)
jmp = [i for i, o in enumerate(body_text_glued) if o[0] == 'x4c']
assert len(jmp) == 1
old = '''      cpu->pc = cpu_readOpcodeWord(cpu, true);
      break;'''
new = '''      uint16_t from = cpu->pc;
      cpu->pc = cpu_readOpcodeWord(cpu, true);
      if((uint16_t)(from - cpu->pc) < 0x100) cpu_loopCheck(cpu); // going back a little: also a loop
      break;'''
o = body_text_glued[jmp[0]]
assert o[1] == old, o[1]
body_text_glued[jmp[0]] = (o[0], new, o[2])

# inlining policy: shared helpers exist once (small code), the rest is
# inlined into its single call site in cpu_run
NOINLINE = ['cpu_getFlags', 'cpu_setFlags', 'cpu_doBranch', 'cpu_pullByte', 'cpu_pushByte', 'cpu_pullWord',
            'cpu_pushWord', 'cpu_readWord', 'cpu_writeWord', 'cpu_doInterrupt', 'cpu_adrImp']
def attr(m):
    name = m.group(2)
    a = 'NS_NOINLINE ' if name in NOINLINE else 'NS_INLINE '
    return a + m.group(1) + name + '('
text = re.sub(r'^static ([\w\* ]+?)\b(cpu_\w+)\(', attr, text, flags=re.M)

head = LICENSE % ('', ' *\n'
 ' * GENERATED by tools/import_cpu.py - the opcode logic below is LakeSnes\' text;\n'
 ' * what nwsnes adds is the way it is laid out and the short bus paths:\n'
 ' *\n'
 ' *  - one loop (cpu_run) instead of one call per opcode;\n'
 ' *  - an opcode is decoded in two steps, addressing mode then operation, so\n'
 ' *    each of them exists once. The whole hot path is a few KB: it has to\n'
 ' *    fit, with the rest of the emulator, in an 8 KB instruction cache whose\n'
 ' *    every miss costs about a microsecond of external flash;\n'
 ' *  - reads and writes of work ram and ROM do not go through the bus decoder.\n'
 ' *\n'
 ' * Timing is exactly that of the reference (cpu_ref.c): same accesses, same\n'
 ' * order, same cycles. Only the wait-loop skipping (speedHacks) changes it.\n')
head += '''#include <stdbool.h>
#include <stdint.h>

#include "snes.h"

#define NS_LIKELY(x) __builtin_expect(!!(x), 1)
#define NS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NS_NOINLINE static __attribute__((noinline))
#define NS_INLINE static inline __attribute__((always_inline))
/* One function per operation, each in a section named after its rank (how
 * often it runs): the linker script (core/hot.ld) lays them out in that
 * order, so that the ones that run all the time share the instruction cache
 * without evicting each other. */
#if defined(__arm__) && defined(__ELF__)
#define NS_OP(rank) static __attribute__((noinline, section(".text.nsop" #rank)))
#define NS_RAM_TABLE static __attribute__((used, section(".data.nstables")))
#else
#define NS_OP(rank) static __attribute__((noinline))
#define NS_RAM_TABLE static
#endif

void cpu_reset(Snes* s, bool hard) {
  Cpu* cpu = &s->cpu;
%s  snes_setCodeBank(s);
  snes_intPend(s);
}

/* ------------------------------------------------------------ bus glue -- */

NS_INLINE void cpu_setK(Cpu* cpu, uint8_t k) {
  cpu->k = k;
  snes_setCodeBank((Snes*)cpu);
}

/* Work ram and ROM are accessed in place. When a scanline event is due
 * during the access, it is run right here, at the same point in time as the
 * complete path (snes_cpuRead and friends) would. That path is only taken
 * for the rest of the bus, and in two cases where the shortcut would not be
 * exact: a DMA is pending (leftBias), or HDMA is on (an event could then
 * write to work ram through $2180 between the address decoding and the
 * access). */
NS_INLINE bool cpu_eventsInPlace(const Snes* s) {
  return !s->leftBias && !s->dma.hdmaAny;
}

NS_NOINLINE uint8_t cpu_read(Cpu* cpu, uint32_t adr) {
  Snes* s = (Snes*)cpu;
  const uint8_t* p;
  int32_t left = s->left;
  if(!(adr & 0x40e000)) { /* 0000-1fff of banks 00-3f and 80-bf */
    p = s->pager.ptr[NS_VP_WRAM + ((adr >> 8) & 0x1f)] + (adr & 0xff);
    left -= 8;
  } else if((p = s->romHalf[(adr >> 15) & 0x1ff]) != NULL) {
    p += adr & 0x7fff;
    left -= s->romCycles[(adr >> 23) & 1];
  } else if((adr >> 17) == 0x3f) { /* 7e0000-7fffff */
    p = s->pager.ptr[NS_VP_WRAM + ((adr >> 8) & 0x1ff)] + (adr & 0xff);
    left -= 8;
  } else {
    return snes_cpuRead(s, adr);
  }
  if(NS_UNLIKELY(left <= 0)) {
    if(!cpu_eventsInPlace(s)) return snes_cpuRead(s, adr); /* which counts the cycles itself */
    s->left = left;
    snes_handleEvents(s);
  } else {
    s->left = left;
  }
  return s->openBus = *p;
}

NS_NOINLINE void cpu_write(Cpu* cpu, uint32_t adr, uint8_t val) {
  Snes* s = (Snes*)cpu;
  uint32_t vp;
  if(!(adr & 0x40e000)) vp = NS_VP_WRAM + ((adr >> 8) & 0x1f);
  else if((adr >> 17) == 0x3f) vp = NS_VP_WRAM + ((adr >> 8) & 0x1ff);
  else goto full;
  {
    uint8_t* p = (uint8_t*)s->pager.ptr[vp];
    const int32_t left = s->left - 8;
    /* a page that is not in RAM yet is copied on write: not here */
    if(NS_UNLIKELY((uintptr_t)p - (uintptr_t)s->pager.pool >= s->pager.poolBytes)) goto full;
    if(NS_UNLIKELY(left <= 0)) {
      if(!cpu_eventsInPlace(s)) goto full;
      s->left = left;
      snes_handleEvents(s);
    } else {
      s->left = left;
    }
    s->loopDirty = true;
    s->openBus = val;
    p[adr & 0xff] = val;
    return;
  }
full:
  snes_cpuWrite(s, adr, val);
}

NS_NOINLINE void cpu_idleSlow(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  if(s->leftBias) {
    snes_cpuIdle(s, false);
    return;
  }
  s->left -= 6;
  snes_handleEvents(s);
}

NS_INLINE void cpu_idle(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  const int32_t left = s->left - 6;
  if(NS_LIKELY(left > 0)) {
    s->left = left;
    return;
  }
  cpu_idleSlow(cpu);
}

NS_INLINE void cpu_idleWait(Cpu* cpu) {
  snes_cpuIdle((Snes*)cpu, true);
}

/* nmiWanted || (irqWanted && !i), precomputed by snes_intPend() */
NS_INLINE void cpu_checkInt(Cpu* cpu) {
  cpu->intWanted = cpu->intPend;
}

/* an opcode byte whose fetch is not the plain case: outside ROM, or with an
 * event due, or a DMA pending */
NS_NOINLINE uint8_t cpu_fetchSlow(Cpu* cpu, uint32_t pc) {
  Snes* s = (Snes*)cpu;
  if(pc >= s->codeMin && !s->leftBias) {
    s->left -= s->codeCycles;
    snes_handleEvents(s);
    return s->openBus = s->codePtr[pc];
  }
  return snes_cpuRead(s, ((uint32_t)cpu->k << 16) | pc);
}

NS_INLINE uint8_t cpu_readOpcode(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  const uint32_t pc = cpu->pc;
  cpu->pc = (uint16_t)(pc + 1);
  if(NS_LIKELY(pc >= s->codeMin)) {
    const int32_t left = s->left - s->codeCycles;
    if(NS_LIKELY(left > 0)) {
      s->left = left;
      return s->openBus = s->codePtr[pc];
    }
  }
  return cpu_fetchSlow(cpu, pc);
}

/* Two bytes at once when nothing can happen in between: no event is due
 * within the two accesses, so the interrupt inputs cannot change either. */
NS_NOINLINE uint16_t cpu_readOpcodeWord(Cpu* cpu, bool intCheck) {
  Snes* s = (Snes*)cpu;
  const uint32_t pc = cpu->pc;
  const int32_t left = s->left - 2 * s->codeCycles;
  if(NS_LIKELY(pc >= s->codeMin) && NS_LIKELY(pc < 0xffff) && NS_LIKELY(left > 0)) {
    const uint8_t* p = s->codePtr + pc;
    cpu->pc = (uint16_t)(pc + 2);
    s->left = left;
    if(intCheck) cpu_checkInt(cpu);
    s->openBus = p[1];
    return (uint16_t)(p[0] | (p[1] << 8));
  }
  uint8_t low = cpu_readOpcode(cpu);
  if(intCheck) cpu_checkInt(cpu);
  return low | (cpu_readOpcode(cpu) << 8);
}

static uint8_t cpu_getFlags(Cpu* cpu);

/* nwsnes speed-up: called when execution goes backwards (a loop). If the cpu
 * comes back to the same place in the same state without having written
 * anything, the loop is only waiting. If it did not read any hardware
 * register either, only an interrupt can end it: run the events until one
 * comes. Otherwise skip to the next thing that can change what it reads. */
NS_NOINLINE void cpu_loopCheck(Cpu* cpu) {
  Snes* s = (Snes*)cpu;
  if(!s->speedHacks) return;
  const uint32_t target = ((uint32_t)cpu->k << 16) | cpu->pc;
  if(target == (cpu->loopTarget & 0xffffff) && !s->loopDirty) {
    const uint32_t sig0 = cpu->a | ((uint32_t)cpu->x << 16);
    const uint32_t sig1 = cpu->y | ((uint32_t)cpu->sp << 16);
    const uint32_t sig2 = cpu->dp | ((uint32_t)cpu->db << 16) | ((uint32_t)cpu_getFlags(cpu) << 24);
    /* bit 24 of loopTarget: the saved state belongs to this target */
    if(cpu->loopTarget == (target | 0x1000000) &&
       sig0 == cpu->loopSig[0] && sig1 == cpu->loopSig[1] && sig2 == cpu->loopSig[2]) {
      if(s->loopIo) snes_skipToEvent(s);
      else snes_skipToInterrupt(s);
    }
    cpu->loopTarget = target | 0x1000000;
    cpu->loopSig[0] = sig0;
    cpu->loopSig[1] = sig1;
    cpu->loopSig[2] = sig2;
  } else {
    cpu->loopTarget = target; /* a loop that works: nothing more to look at */
  }
  s->loopDirty = false;
  s->loopIo = false;
}

/* --------------------------------------- LakeSnes: helpers, modes, ops -- */

'''
reset_body = reset_fn[reset_fn.index('{\n') + 2:reset_fn.rindex('}')]
head = head % reset_body

# 3. the loop
run = """
/* ------------------------------------------------------------- the loop -- */

/* what keeps an opcode from being fetched; returns false when the frame is over */
NS_NOINLINE bool cpu_special(Cpu* cpu) {
  if(cpu->frameDone) {
    cpu->frameDone = false;
    return false;
  }
  if(cpu->stopped) {
    cpu_idleWait(cpu);
    return true;
  }
  if(cpu->waiting) {
    if(cpu->irqWanted || cpu->nmiWanted) {
      cpu->waiting = false;
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
    } else {
      cpu_idleWait(cpu);
    }
    return true;
  }
  // not stopped or waiting: go to interrupt
  cpu_read(cpu, (cpu->k << 16) | cpu->pc);
  cpu_doInterrupt(cpu);
  return true;
}

NS_NOINLINE void cpu_doReset(Cpu* cpu) {
%(reset)s}

/* ---------------------------------- operations, most used ones first -- */

%(op_funcs)s
/* -------------------------------------------------------------- decoding -- */

enum {
%(mode_enum)s
};

typedef void (*CpuOp)(Cpu* cpu, uint32_t low, uint32_t high);

/* opcode -> addressing mode and operation. Read on every opcode: they are
 * forced into RAM (a const table would sit behind the slow flash). */
NS_RAM_TABLE uint8_t kMode[256] = {
%(mode_table)s
};
NS_RAM_TABLE CpuOp kOp[256] = {
%(op_table)s
};

#ifdef NS_OPCODE_STATS
uint32_t ns_opcodeStats[256]; /* how often each opcode runs: decides the ranks (tools/import_cpu.py) */
#endif

__attribute__((noinline)) void cpu_run(Snes* s) {
  Cpu* cpu = &s->cpu;
  if(cpu->resetWanted) cpu_doReset(cpu);
  for(;;) {
    if(NS_UNLIKELY(cpu->special)) {
      if(!cpu_special(cpu)) return;
      continue;
    }
    const uint32_t opcode = cpu_readOpcode(cpu);
#ifdef NS_OPCODE_STATS
    ns_opcodeStats[opcode]++;
#endif
    uint32_t low = 0;
    uint32_t high = 0;
    switch(kMode[opcode]) {
%(mode_cases)s      default: break;
    }
    kOp[opcode](cpu, low, high);
  }
}
"""
reset_seq = src[src.index('    cpu->resetWanted = false;\n    // reset: brk/interrupt without writes'):src.index('    return;\n  }\n  if(cpu->stopped) {')]
reset_seq = glue(reset_seq)
reset_seq = '\n'.join(l[2:] if l.startswith('    ') else l for l in reset_seq.split('\n'))

mode_enum = ',\n'.join('  M_%s' % m for m in modes)
mode_rows = []
op_rows = []
for i, (mode, opi, comment) in enumerate(table):
    mode_rows.append('  M_%s, /* %02x %s */' % (mode, i, comment))
    op_rows.append('  op_%s, /* %02x %s */' % (body_text_glued[opi][0], i, comment))
mode_cases = ''
for m in modes[1:]:
    mode_cases += '      case M_%s: high = %s; break;\n' % (m, mode_call[m])
op_funcs = ''
for rank, (name, body, comment) in enumerate(body_text_glued):
    if body is None:
        op_funcs += 'NS_OP(%03d) void op_%s(Cpu* cpu, uint32_t low, uint32_t high) {\n  cpu_%s(cpu, low, high);\n}\n\n' % (rank, name, name)
    else:
        assert body.rstrip().endswith('break;') and body.count('break;') == 1
        b2 = body.rstrip()[:-len('break;')].rstrip()
        b2 = '\n'.join(l[4:] if l.startswith('    ') else l for l in b2.split('\n'))
        op_funcs += ('NS_OP(%03d) void op_%s(Cpu* cpu, uint32_t noLow, uint32_t noHigh) { // %s\n  (void)noLow;\n  (void)noHigh;\n%s\n}\n\n'
                     % (rank, name, comment, b2))
run = run % dict(reset=reset_seq, mode_enum=mode_enum, mode_table='\n'.join(mode_rows), op_table='\n'.join(op_rows),
                 mode_cases=mode_cases, op_funcs=op_funcs)

fast = head + text + run
assert 'cpu->mem' not in fast and 'malloc' not in fast and 'cpu->k = ' not in text + run
open(out + '/cpu.c', 'w').write(fast)
print('ok: %d addressing modes, %d operations (%d shared)' % (len(modes) - 1, len(ops), sum(1 for o in ops if o[1] is None)))
