# nwsnes - SNES core for the NumWorks calculator
#
#   make host      PC test harness (build/nwsnes-host), with the reference renderer
#   make nwa       the calculator app (build/nwsnes.nwa), needs arm-none-eabi-gcc + node
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

.PHONY: all host nwa wasm test clean
all: host

host: $(BUILD)/nwsnes-host

$(BUILD)/nwsnes-host: $(CORE) core/ppu_ref.c $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(CORE) core/ppu_ref.c $(PC) host/main.c -o $@

# same thing with sanitizers, to catch memory errors in the core
$(BUILD)/nwsnes-host-asan: $(CORE) core/ppu_ref.c $(PC) host/main.c $(HEADERS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	  $(CORE) core/ppu_ref.c $(PC) host/main.c -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
