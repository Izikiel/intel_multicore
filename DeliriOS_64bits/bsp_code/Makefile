OBJ=$(KERNEL_OBJ) ../common_code/gdt.o ../common_code/ports.o ../common_code/mmu.o \
	idt.o isr.o bsp.o ../common_code/contextManager.o \
	../common_code/multicore_common.o \
	utils.o kmain64.o ../common_code/i386.o ../common_code/timer.o multicore_boot.o \
	cmos.o console.o ../common_code/sort_code.o bsp_execute_code.o test_code.o \
	../common_code/screen_utils.o ../common_code/init_ioapic.o

CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include \
  -mno-sse -mno-sse2 -mno-sse3 -mno-sse4.1 -mno-sse4.2

#variables
KERNEL_SRC=kernel.asm
KERNEL_OBJ=kernel.o
KERNEL_BIN=kernel.bin
LINKSCRIPT=linker_script.ld

CODESIZE=262144
KERNELSIZE=262144

#flags ans binaries
CC=gcc
NASM=nasm
NASMFLAGS=-felf64 -I../include/
LD=ld
LDFLAGS=-static -melf_x86_64 -T $(LINKSCRIPT)

QUIET = @

.PHONY=clean all image

all: console_clear clean common_code kernel.bin

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(OBJ)
	@echo ''
	@echo 'Linkeando kernel...'
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo ''
	@echo 'Copiando el binario a grub-init...'
	cp kernel.bin ../grub-init/kernel64.bin64
	@echo ''

console_clear:
	reset

common_code:
	cd ../common_code; make

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	cd ../common_code; make clean
	@echo ''
