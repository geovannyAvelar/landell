CROSS   ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
BUILD   ?= build

CFLAGS  = -mcpu=cortex-m0 -mthumb -O1 -g -Wall -Wextra \
          -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections
LDFLAGS = -nostdlib -nostartfiles -T link.ld -Wl,--gc-sections \
          -Wl,-Map=$(BUILD)/firmware.map

all: $(BUILD)/firmware.bin

$(BUILD)/firmware.elf: startup.c main.c link.ld | $(BUILD)
	$(CC) $(CFLAGS) startup.c main.c $(LDFLAGS) -o $@

$(BUILD)/firmware.bin: $(BUILD)/firmware.elf
	$(OBJCOPY) -O binary $< $@

disasm: $(BUILD)/firmware.elf
	$(OBJDUMP) -d -M force-thumb $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all disasm clean
