DEPEND = Makefile\
	kernel.asm isr.asm cmos.asm i386.asm\
	contextManager.c gdt.c idt.c mmu.c ports.c utils.c kmain64.c console.c\
	timer.c multicore.c \
	../include/contextManager.h ../include/defines.h ../include/gdt.h ../include/idt.h\
	../include/isr.h ../include/mmu.h ../include/ports.h ../include/vargs.h\
	../include/types.h ../include/utils.h ../include/kmain64.h ../include/console.h\
	../include/irq.h ../include/i386.h ../include/timer.h ../include/multicore.h

OBJ=$(KERNEL_OBJ) ../common_code/gdt.o ../common_code/ports.o ../common_code/mmu.o \
	../common_code/idt.o ../common_code/isr.o ../common_code/contextManager.o \
	utils.o kmain64.o ../common_code/i386.o ../common_code/timer.o multicore.o \
	cmos.o console.o

CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include

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

all: console_clear clean common_code kernel.bin $(DEPEND)

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
