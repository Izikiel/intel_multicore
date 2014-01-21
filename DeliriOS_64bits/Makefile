MCOPY=mcopy

BOOTLOADER_SRC=bootsector.asm
BOOTLOADER_BIN=bootsector.bin

#DISKETTE_IMG=diskette.img
#DISKETTE_LABEL=ORGA2

DISK_IMG=hdd.img
DISK_LABEL=DeliriOS

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
	@echo 'Corriendo bochs'
	./bochs/bin/bochs -q -f bochsrc

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

bootloader: $(BOOTLOADER_SRC)
	@echo 'Compilando el bootloader...'
	nasm -fbin $(BOOTLOADER_SRC) -o $(BOOTLOADER_BIN)
	@echo ''

kernel.bin: $(OBJ)
	@echo ''
	@echo 'Linkeando kernel...'
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo 'Generando imagen del kernel...'
	mv kernel.bin kernel.bin.tmp
	dd if=/dev/zero of=kernel.bin bs=1 count=$(KERNELSIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	dd if=kernel.bin.tmp of=kernel.bin bs=1 count=$(CODESIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	rm kernel.bin.tmp
	@echo ''

image: bootloader kernel.bin $(DEPEND)
#	@echo 'Generando una imagen de diskette vacía...'
#	dd bs=512 count=2880 if=/dev/zero of=$(DISKETTE_IMG)
#	@echo ''
#
#	@echo 'Formateando la imagen de diskette...'
#	sudo mkfs.msdos -F 12 $(DISKETTE_IMG) -n "$(DISKETTE_LABEL)"
#	@echo ''
#
#	@echo 'Escribiendo el bootsector en la imagen de diskette...'
#	dd if=$(BOOTLOADER_BIN) of=$(DISKETTE_IMG) count=1 seek=0 conv=notrunc
#	@echo ''

#	@if [ -f $(KERNEL_BIN) ] ; then \
#		echo 'Copiando el $(KERNEL_BIN) a la imagen de diskette\n'\
#		$(MCOPY) -i $(DISKETTE_IMG) $(KERNEL_BIN) ::/ ;\
#		$(MCOPY) -i $(DISKETTE_IMG) $(KERNEL_BIN) ::/ ;\
#	else \
#		echo 'No se copio el kernel porque no hay' ;\
#	fi

	@echo 'Generando una imagen de disco vacía...'
	dd bs=1024 count=20480 if=/dev/zero of=$(DISK_IMG)
	@echo ''

	@echo 'Formateando la imagen de disco en fat16...'
	sudo mkfs.msdos -F 16 $(DISK_IMG) -n "$(DISK_LABEL)"
	@echo ''

	@echo 'Escribiendo el bootsector en la imagen de disco...'
	dd if=$(BOOTLOADER_BIN) of=$(DISK_IMG) count=1 seek=0 conv=notrunc
	@echo ''

#	@if [ -f $(KERNEL_BIN) ] ; then \
#		echo 'Copiando el $(KERNEL_BIN) a la imagen de diskette\n'\
#		$(MCOPY) -i $(DISK_IMG) $(KERNEL_BIN) ::/ ;\
#		$(MCOPY) -i $(DISK_IMG) $(KERNEL_BIN) ::/ ;\
#	else \
#		echo 'No se copio el kernel porque no hay' ;\
#	fi

console_clear:
	reset

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	rm -f *.tmp
	#rm -f $(DISKETTE_IMG)
	rm -f $(DISK_IMG)
	@echo ''

usb: console_clear clean image
	@echo 'Escribiendo el bootsector en el usb...'
	sudo dd if=$(BOOTLOADER_BIN) of=/dev/sdb count=1 seek=0 conv=notrunc
	@echo ''
