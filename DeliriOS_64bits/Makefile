MCOPY=mcopy

KERNEL_SRC=kernel.asm
KERNEL_OBJ=kernel.o
KERNEL_BIN=kernel.bin
LINKSCRIPT=linker_script.ld

DEPEND = Makefile\
	kernel.asm isr.asm cmos.asm i386.asm\
	contextManager.c gdt.c idt.c mmu.c ports.c utils.c kmain64.c \
	timer.c multicore.c \
	./include/contextManager.h ./include/defines.h ./include/gdt.h ./include/idt.h\
	./include/isr.h ./include/mmu.h ./include/ports.h ./include/vargs.h\
	./include/types.h ./include/utils.h ./include/kmain64.h \
	./include/irq.h ./include/i386.h ./include/timer.h ./include/multicore.h

OBJ=$(KERNEL_OBJ) gdt.o ports.o mmu.o idt.o isr.o contextManager.o utils.o kmain64.o \
	i386.o timer.o multicore.o cmos.o\
	ap_startup_code.o mergesort.o

DISK_IMG=diskette.img
DISK_IMG_BZ=diskette.img.bz2

CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I./include

CODESIZE=262144
KERNELSIZE=262144

#variables
CC=gcc
NASM=nasm
NASMFLAGS=-felf64 -I./include/
LD=ld
LDFLAGS=-static -melf_x86_64 -T $(LINKSCRIPT)

QUIET = @

.PHONY=clean all image

all: console_clear clean image

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(OBJ)
	@echo 'Linkeando kernel...'
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo 'Generando imagen del kernel...'
	mv kernel.bin kernel.bin.tmp
	dd if=/dev/zero of=kernel.bin bs=1 count=$(KERNELSIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	dd if=kernel.bin.tmp of=kernel.bin bs=1 count=$(CODESIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	rm kernel.bin.tmp
	@echo ''

image: kernel.bin $(DEPEND) $(DISK_IMG)
	@echo 'Copiando el $(KERNEL_BIN) a la imagen de diskette'
	$(MCOPY) -o -i $(DISK_IMG) $(KERNEL_BIN) ::/
	@echo 'Corriendo bochs'
	./bochs/bin/bochs -q -f bochsrc

$(DISK_IMG): $(DISK_IMG_BZ)
	bzip2 -dk $(DISK_IMG_BZ)

console_clear:
	reset

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	rm -f *.tmp
	rm -f $(DISK_IMG)
