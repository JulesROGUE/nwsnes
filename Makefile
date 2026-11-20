# nwsnes - SNES core for the NumWorks calculator
#
#   make host      PC test harness (build/nwsnes-host), with the reference renderer
#   make nwa       the calculator app (build/nwsnes.nwa), needs arm-none-eabi-gcc + node
#   make sim       the calculator front end running on the PC (build/numworks-sim)
#   make bench PACK=x  instructions per frame on an emulated Cortex-M7 (needs qemu-system-arm)
#   make wasm      the core + profiler as WebAssembly, for the snes2nw PC tool (needs zig)
#   make test      runs the checks on the test ROM (make testrom rebuilds that ROM, needs cc65)
#   make clean

BUILD := build
comma := ,
CORE := core/snes.c core/cpu.c core/dma.c core/ppu.c core/ppu_regs.c core/apu_stub.c core/pager.c core/pack.c
REF := core/ppu_ref.c core/cpu_ref.c
PC := pc/profiler.c pc/romhdr.c
HEADERS := $(wildcard core/*.h pc/*.h)

HOST_CC ?= cc
HOST_CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS += -DNS_WITH_REFERENCE

.PHONY: all host nwa nwa-check diag sim bench wasm testrom test clean
all: host

host: $(BUILD)/nwsnes-host

$(BUILD)/nwsnes-host: $(CORE) $(REF) $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(CORE) $(REF) $(PC) host/main.c -o $@

# same thing with sanitizers, to catch memory errors in the core
$(BUILD)/nwsnes-host-asan: $(CORE) $(REF) $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	  $(CORE) $(REF) $(PC) host/main.c -o $@

# ---- calculator app -------------------------------------------------------
NWLINK ?= npx --yes -- nwlink@1.0.0
ARM_CC ?= arm-none-eabi-gcc
ARM_CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
ARM_CFLAGS += $(shell $(NWLINK) eadk-cflags-device)
ARM_CFLAGS += -fdata-sections -ffunction-sections
ARM_CFLAGS += -flto -fno-fat-lto-objects -fwhole-program -fvisibility=internal
ARM_LDFLAGS = -Wl,--relocatable -nostartfiles --specs=nano.specs
ARM_LDFLAGS += -Wl,-e,main -Wl,-u,eadk_app_name -Wl,-u,eadk_app_icon -Wl,-u,eadk_api_level
ARM_LDFLAGS += -Wl,--gc-sections -flinker-output=nolto-rel
# function order in flash, see numworks/app.ld
ARM_LDFLAGS += -Wl,-T,numworks/app.ld

nwa: $(BUILD)/nwsnes.nwa

ARM_OBJS := $(patsubst %.c,$(BUILD)/arm/%.o,$(CORE) numworks/main.c)

$(BUILD)/arm/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) $(if $(filter core/ppu.c,$<),-O3) -c $< -o $@

$(BUILD)/icon.o: numworks/icon.png | $(BUILD)
	$(NWLINK) png-icon-o $< $@

$(BUILD)/nwsnes.nwa: $(ARM_OBJS) $(BUILD)/icon.o numworks/app.ld core/hot.ld
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) $(ARM_OBJS) $(BUILD)/icon.o -o $@

# ---- diagnostic app: measures the calculator itself (see numworks/diag.c) ---
diag: $(BUILD)/nwsnes-diag.nwa

$(BUILD)/nwsnes-diag.nwa: numworks/diag.c $(BUILD)/icon.o | $(BUILD)
	python3 -c "import random; r = random.Random(7); open('$(BUILD)/diag_blob.bin', 'wb').write(bytes(r.randrange(256) for _ in range(262144)))"
	printf '.section .rodata.diag_blob\n.global diag_blob\n.balign 32\ndiag_blob:\n.incbin "$(BUILD)/diag_blob.bin"\n' > $(BUILD)/diag_blob.S
	$(ARM_CC) -std=c11 -O2 -Wall $(shell $(NWLINK) eadk-cflags-device) -fdata-sections -ffunction-sections \
	  $(filter-out -flinker-output=nolto-rel -Wl$(comma)-T$(comma)numworks/app.ld,$(ARM_LDFLAGS)) numworks/diag.c $(BUILD)/diag_blob.S $(BUILD)/icon.o -o $@
	$(NWLINK) nwa-elf $@ $(BUILD)/nwsnes-diag.elf
	arm-none-eabi-size -A $(BUILD)/nwsnes-diag.elf | grep -E "\.text|\.rodata |\.data|\.bss"

# links the app the way the installer does (with an empty game), to check
# that it links and to see how much RAM its own data takes
nwa-check: $(BUILD)/nwsnes.nwa
	printf 'none' > $(BUILD)/empty.pack
	$(NWLINK) nwa-elf --external-data $(BUILD)/empty.pack $< $(BUILD)/nwsnes.elf
	arm-none-eabi-size -A $(BUILD)/nwsnes.elf | grep -E "section|\.text|\.rodata|\.data|\.bss"

# ---- the calculator front end, run on the PC against stand-in EADK calls ----
EADK_INC = $(shell $(NWLINK) eadk-cflags-device | sed 's/.*"-I\([^"]*\)".*/\1/')
sim: $(BUILD)/numworks-sim
$(BUILD)/numworks-sim: $(CORE) numworks/main.c host/numworks_sim.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -DNS_SIM -DPLATFORM_DEVICE=1 -Dmain=app_main -I"$(EADK_INC)" -c numworks/main.c -o $(BUILD)/sim_main.o
	$(HOST_CC) $(HOST_CFLAGS) -DPLATFORM_DEVICE=1 -I"$(EADK_INC)" $(CORE) $(REF) host/numworks_sim.c $(BUILD)/sim_main.o -o $@

# ---- WebAssembly module for the PC tool -----------------------------------
# "pip install ziglang" provides zig as "python3 -m ziglang"
ZIG ?= python3 -m ziglang
wasm: $(BUILD)/nwsnes.wasm

$(BUILD)/nwsnes.wasm: $(CORE) $(PC) wasm/api.c $(HEADERS) | $(BUILD)
	$(ZIG) cc -target wasm32-wasi -mexec-model=reactor -O2 -s -std=c11 -Wall \
	  $(CORE) $(PC) wasm/api.c -o $@

# ---- instruction count on an emulated Cortex-M7 (see bench/main.c) --------
# make bench PACK=game.pack [FRAMES=600] [SEED=7] [POOL_KB=90]
# CACHESIM=path/to/libcachesim.so : optional QEMU plugin modelling the 8 KB
# caches of the calculator's processor (see docs/CONCEPTION.md)
CACHESIM ?=
FRAMES ?= 600
SEED ?= 7
POOL_KB ?= 90
SKIP ?= 0
# INPUTS=file : binary file of little-endian u16 button states, one per frame
# (tools/script2bin.py converts a text input script); default: button masher
INPUTS ?=
BENCH_CFLAGS = -std=c11 -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -fdata-sections -ffunction-sections -flto \
  -DBENCH_FRAMES=$(FRAMES) -DBENCH_SEED=$(SEED) -DBENCH_POOL_KB=$(POOL_KB) -DBENCH_SKIP=$(SKIP) $(BENCH_DEFS)

bench: $(CORE) bench/main.c bench/startup.c bench/mps2.ld $(HEADERS) | $(BUILD)
	@test -n "$(PACK)" || (echo "usage: make bench PACK=game.pack"; exit 1)
	printf '.section .rodata\n.global bench_pack\n.global bench_pack_size\n.balign 4\nbench_pack:\n.incbin "%s"\nbench_pack_end:\n.balign 4\nbench_pack_size:\n.word bench_pack_end - bench_pack\n' "$(PACK)" > $(BUILD)/bench_pack.S
	printf '.global bench_inputs\n.global bench_inputs_size\n.balign 4\nbench_inputs:\n' >> $(BUILD)/bench_pack.S
	if [ -n "$(INPUTS)" ]; then printf '.incbin "%s"\n' "$(INPUTS)" >> $(BUILD)/bench_pack.S; fi
	printf 'bench_inputs_end:\n.balign 4\nbench_inputs_size:\n.word bench_inputs_end - bench_inputs\n' >> $(BUILD)/bench_pack.S
	@mkdir -p $(BUILD)/bench
	for f in $(CORE) bench/main.c bench/startup.c; do \
	  o=$(BUILD)/bench/$$(basename $$f .c).o; \
	  case $$f in core/ppu.c) opt=-O3;; *) opt=;; esac; \
	  $(ARM_CC) $(BENCH_CFLAGS) $$opt -c $$f -o $$o || exit 1; \
	done
	$(ARM_CC) $(BENCH_CFLAGS) $(BUILD)/bench/*.o $(BUILD)/bench_pack.S \
	  -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -T bench/mps2.ld -o $(BUILD)/bench.elf
	qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -monitor none -serial none \
	  -icount shift=0 -semihosting-config enable=on,target=native -kernel $(BUILD)/bench.elf \
	  $(if $(CACHESIM),-plugin $(CACHESIM)$(comma)marker=0x$(shell arm-none-eabi-nm $(BUILD)/bench.elf | grep ' bench_marker$$' | cut -d' ' -f1))

# ---- test ROM (original program and graphics, see tests/rom) and tests -----
# A prebuilt copy is kept in tests/rom/nwsnes-test.sfc; "make testrom"
# rebuilds it from source (needs python3 and cc65).
TESTROM := tests/rom/nwsnes-test.sfc
testrom:
	mkdir -p $(BUILD)/testrom
	python3 tests/rom/gen_assets.py $(BUILD)/testrom
	ca65 --cpu 65816 --bin-include-dir $(BUILD)/testrom tests/rom/test.s -o $(BUILD)/testrom/test.o
	ld65 -C tests/rom/lorom.cfg $(BUILD)/testrom/test.o -o $(TESTROM)
	python3 tests/rom/fix_checksum.py $(TESTROM)

test: host
	sh tests/run.sh

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
