# i8088 slow-clock signal generator — one-command build & flash over USB.
#
# Reuses the vendored SDK/toolchain/picotool from the sibling magner150 project
# (see CMakeLists REF_TOOLCHAIN), so nothing is downloaded. cmake/ninja are host
# tools (Homebrew). Override any var on the command line, e.g. `make PICOTOOL=...`.
REF_TOOLCHAIN := /Users/andreykirillov/Desktop/projects/mcu/pico/projects/c/magner150_vga_rp2350/toolchain
CMAKE         := cmake
NINJA         := ninja
PICOTOOL      := $(REF_TOOLCHAIN)/picotool/picotool/picotool

BUILD_DIR := build
UF2       := $(BUILD_DIR)/clkgen.uf2

# Ensure Homebrew bin is searchable for cmake/ninja/arm-none-eabi-gcc/picotool.
export PATH := /opt/homebrew/bin:$(PATH)

.PHONY: uf2 configure build-pico flash flash-pico flash-bootsel reboot clean help

.DEFAULT_GOAL := uf2

help:
	@echo "Targets:"
	@echo "  uf2           build all firmware files (default; no flashing)"
	@echo "  flash         rebuild + flash over USB cable (alias of flash-pico)"
	@echo "  flash-pico    rebuild + flash over USB cable (no BOOTSEL button)"
	@echo "  flash-bootsel rebuild + flash (chip already in BOOTSEL mode)"
	@echo "  reboot        reboot the running chip"
	@echo "  clean         wipe build directory"

configure:
	@$(CMAKE) -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release

uf2: build-pico
	@echo "Firmware files in $(BUILD_DIR)/:"
	@ls -1 $(BUILD_DIR)/clkgen.uf2 $(BUILD_DIR)/clkgen.bin $(BUILD_DIR)/clkgen.hex \
	       $(BUILD_DIR)/clkgen.elf 2>/dev/null || true

build-pico:
	@if [ ! -f $(BUILD_DIR)/build.ninja ]; then $(MAKE) configure; fi
	@$(NINJA) -C $(BUILD_DIR)
	@echo "Built: $(UF2)"

# `make flash` = rebuild + flash over USB cable (alias of flash-pico).
flash: flash-pico

# -u resets the running chip into BOOTSEL via USB; -f flash; -x execute after.
flash-pico: build-pico
	@$(PICOTOOL) load -fx -u $(UF2)

# Fallback when -u fails: hold BOOTSEL + plug USB, then run this.
flash-bootsel: build-pico
	@$(PICOTOOL) load -fx $(UF2)

reboot:
	@$(PICOTOOL) reboot

clean:
	@rm -rf $(BUILD_DIR)
