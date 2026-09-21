CC      = riscv64-linux-gnu-gcc
OBJCOPY = riscv64-linux-gnu-objcopy
QEMU    = qemu-system-riscv32
OPENSBI = /usr/lib/riscv32-linux-gnu/opensbi/generic/fw_dynamic.bin

CFLAGS = -std=c11 -O2 -g3 -Wall -Wextra \
         -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
         -ffreestanding -nostdlib -static -fno-pie -fno-stack-protector \
         -Wl,-Tkernel.ld -Wl,-Map=kernel.map -Wl,--no-dynamic-linker -no-pie \
         -Wl,--build-id=none

SRCS = kernel.c

.PHONY: all run clean

all: kernel.elf

kernel.elf: $(SRCS) kernel.ld
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: kernel.elf
	$(QEMU) -machine virt -m 128M -bios $(OPENSBI) -nographic -serial mon:stdio \
		-kernel kernel.elf

clean:
	rm -f kernel.elf kernel.map
