# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the GameCube adapter blob that is copied into a game's memory
# (see ../gcadapter.cpp). The sources in this folder come from RiftWii
# (https://github.com/KakarottoCake/Riftwii, runtime/ and runtime/pad/),
# unchanged; this is RiftWii's "padan" variant: the WUP-028 adapter alone
# (GCAD_ADAPTER_ONLY) and no HOME Menu by the controller (RT_PAD_NO_HOME).
#
# Freestanding, no libc, position independent: it is linked at two bases
# and the two binaries must be identical; .rodata/.data/.bss must be empty.
#
#   make -f source/gcadapter/runtime/Makefile OUT=<dir>
#
# produces <dir>/gcpad.bin.
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

SRC     := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
OUT     ?= build/gcpad
BLOB    := $(OUT)/gcpad.bin

CFLAGS  := -Os -mcpu=750 -meabi -msoft-float -msdata=none \
           -ffreestanding -nostdlib -fno-builtin -fno-jump-tables \
           -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables \
           -fno-stack-protector -fno-common -ffunction-sections -std=c99 -Wall -Wextra -Wpedantic -Werror \
           -DRT_TARGET_PPC -DGCAD_ADAPTER_ONLY -DRT_PAD_NO_HOME -I$(SRC)
LDFLAGS := -nostdlib -nostartfiles -static -Wl,--build-id=none -Wl,--gc-sections -T $(SRC)/pad.ld

OBJS    := $(OUT)/pad_entry.o $(OUT)/pad_hook.o $(OUT)/rtgcad.o $(OUT)/rtleave.o $(OUT)/rtrestart.o
HEADERS := $(wildcard $(SRC)/*.h)

.PHONY: all
all: $(BLOB)

$(OUT):
	@mkdir -p $@

$(OUT)/%.o: $(SRC)/%.c $(HEADERS) | $(OUT)
	@echo gcpad: $(notdir $<)
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(SRC)/%.S $(HEADERS) | $(OUT)
	@echo gcpad: $(notdir $<)
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/gcpad.elf: $(OBJS) $(SRC)/pad.ld
	@$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym,RT_BASE=0x00000000 -o $@ $(OBJS) -lgcc
	@$(SIZE) -A $@ | awk '$$1 ~ /^\.(rodata|data|bss)$$/ && $$2 != 0 { print "gcpad: section " $$1 " is not empty (" $$2 " bytes)"; bad = 1 } END { exit bad }'

$(OUT)/gcpad_alt.elf: $(OBJS) $(SRC)/pad.ld
	@$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym,RT_BASE=0x12340000 -o $@ $(OBJS) -lgcc

$(BLOB): $(OUT)/gcpad.elf $(OUT)/gcpad_alt.elf
	@$(OBJCOPY) -O binary $(OUT)/gcpad.elf $@
	@$(OBJCOPY) -O binary $(OUT)/gcpad_alt.elf $(OUT)/gcpad_alt.bin
	@cmp -s $@ $(OUT)/gcpad_alt.bin || { echo "gcpad: the blob differs between link bases; not position independent"; rm -f $@; exit 1; }
	@echo "gcpad: GameCube adapter blob, $$(stat -c %s $@) bytes"
