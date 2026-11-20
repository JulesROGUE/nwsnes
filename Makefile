# nwsnes - SNES core for the NumWorks calculator
#
#   make host      PC test harness (build/nwsnes-host), with the reference renderer
#   make nwa       the calculator app (build/nwsnes.nwa), needs arm-none-eabi-gcc + node
#   make bench PACK=x  instructions per frame on an emulated Cortex-M7 (needs qemu-system-arm)
#   make wasm      the core + profiler as WebAssembly, for the snes2nw PC tool (needs zig)
#   make test      builds the test ROM and runs the checks
#   make clean

BUILD := build
CORE := core/snes.c core/cpu.c core/dma.c core/ppu.c core/ppu_regs.c core/apu_stub.c core/pager.c core/pack.c
PC := pc/profiler.c pc/romhdr.c
HEADERS := $(wildcard core/*.h pc/*.h)

HOST_CC ?= cc
HOST_CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS += -DNS_WITH_REFERENCE

.PHONY: all host nwa nwa-check bench wasm test clean
all: host

host: $(BUILD)/nwsnes-host

$(BUILD)/nwsnes-host: $(CORE) core/ppu_ref.c $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(CORE) core/ppu_ref.c $(PC) host/main.c -o $@

# same thing with sanitizers, to catch memory errors in the core
$(BUILD)/nwsnes-host-asan: $(CORE) core/ppu_ref.c $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	  $(CORE) core/ppu_ref.c $(PC) host/main.c -o $@

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

nwa: $(BUILD)/nwsnes.nwa

ARM_OBJS := $(patsubst %.c,$(BUILD)/arm/%.o,$(CORE) numworks/main.c)

$(BUILD)/arm/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) $(if $(filter core/ppu.c,$<),-O3) -c $< -o $@

$(BUILD)/icon.o: numworks/icon.png | $(BUILD)
	$(NWLINK) png-icon-o $< $@

$(BUILD)/nwsnes.nwa: $(ARM_OBJS) $(BUILD)/icon.o
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) $^ -o $@

# links the app the way the installer does (with an empty game), to check
# that it links and to see how much RAM its own data takes
nwa-check: $(BUILD)/nwsnes.nwa
	printf 'none' > $(BUILD)/empty.pack
	$(NWLINK) nwa-elf --external-data $(BUILD)/empty.pack $< $(BUILD)/nwsnes.elf
	arm-none-eabi-size -A $(BUILD)/nwsnes.elf | grep -E "section|\.text|\.rodata|\.data|\.bss"

# ---- instruction count on an emulated Cortex-M7 (see bench/main.c) --------
# make bench PACK=game.pack [FRAMES=600] [SEED=7] [POOL_KB=90]
FRAMES ?= 600
SEED ?= 7
POOL_KB ?= 90
BENCH_CFLAGS = -std=c11 -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -fdata-sections -ffunction-sections -flto \
  -DBENCH_FRAMES=$(FRAMES) -DBENCH_SEED=$(SEED) -DBENCH_POOL_KB=$(POOL_KB)

bench: $(CORE) bench/main.c bench/startup.c bench/mps2.ld $(HEADERS) | $(BUILD)
	@test -n "$(PACK)" || (echo "usage: make bench PACK=game.pack"; exit 1)
	printf '.section .rodata\n.global bench_pack\n.global bench_pack_size\n.balign 4\nbench_pack:\n.incbin "%s"\nbench_pack_end:\n.balign 4\nbench_pack_size:\n.word bench_pack_end - bench_pack\n' "$(PACK)" > $(BUILD)/bench_pack.S
	@mkdir -p $(BUILD)/bench
	for f in $(CORE) bench/main.c bench/startup.c; do \
	  o=$(BUILD)/bench/$$(basename $$f .c).o; \
	  case $$f in core/ppu.c) opt=-O3;; *) opt=;; esac; \
	  $(ARM_CC) $(BENCH_CFLAGS) $$opt -c $$f -o $$o || exit 1; \
	done
	$(ARM_CC) $(BENCH_CFLAGS) $(BUILD)/bench/*.o $(BUILD)/bench_pack.S \
	  -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -T bench/mps2.ld -o $(BUILD)/bench.elf
	qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -monitor none -serial none \
	  -icount shift=0 -semihosting-config enable=on,target=native -kernel $(BUILD)/bench.elf

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
