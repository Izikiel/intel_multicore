KERNEL_SRC=ap_full_code.asm
KERNEL_OBJ=ap_full_code.o


OBJ=$(KERNEL_OBJ) ../common_code/gdt.o ../common_code/mmu.o ../common_code/ports.o \
	 idt.o isr.o ../common_code/contextManager.o ../common_code/multicore_common.o \
	../common_code/i386.o ../common_code/timer.o ap_execute_code.o ../common_code/sort_code.o \
	../bsp_code/kmain64.o ../common_code/screen_utils.o ../common_code/check_rax.o\
	../fft/fft.o ../fft/sin.o ../fft/complex.o

LD=ld
LDFLAGS=-static -m elf_x86_64 -T $(LINKSCRIPT)

CC=gcc
CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include

NASM=nasm
NASMFLAGS=-felf64 -I../include/

LINKSCRIPT=linker_script.ld


QUIET = @

.PHONY=clean all image


all: clean common_code fft_code ap_full_code.bin

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

common_code:
	cd ../common_code; make

fft_code:
	cd ../fft; make

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

ap_full_code.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	cp ap_full_code.bin ../grub-init/ap_full_code.bin64

clean:
	rm -rf *.o
	rm -rf *.bin
	cd ../common_code; make clean
	cd ../fft; make clean


