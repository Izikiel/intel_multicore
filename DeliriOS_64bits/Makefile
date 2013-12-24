MCOPY=mcopy

KERNEL_SRC=kernel.asm
KERNEL_OBJ=kernel.o
KERNEL_BIN=kernel.bin

DEPEND = Makefile\
	kernel.asm isr.asm cmos.asm i386.asm\
	contextManager.c gdt.c idt.c mmu.c ports.c screen.c utils.c kmain64.c keyboard.c command.c irq.c\
	./include/contextManager.h ./include/defines.h ./include/gdt.h ./include/idt.h\
	./include/isr.h ./include/mmu.h ./include/ports.h ./include/screen.h ./include/vargs.h\
	./include/types.h ./include/utils.h ./include/kmain64.h ./include/keyboard.h ./include/command.h \
	./include/irq.h ./include/i386.h 
	

OBJ=$(KERNEL_OBJ) gdt.o ports.o mmu.o idt.o isr.o irq.o contextManager.o screen.o utils.o kmain64.o keyboard.o command.o i386.o

DISK_IMG=diskette.img
DISK_IMG_BZ=diskette.img.bz2

CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I./include

CODESIZE=60928 # = 65536 - 0x1200 = 64KB - 0x1200
KERNELSIZE=60928 # = CODESIZE

#Section offsets
SECTION_TEXT_START_OFFSET = 0x1200
SECTION_DATA_START_OFFSET = 0x8000

#variables
CC=gcc
NASM=nasm
NASMFLAGS=-felf64 -I./include/
LD=ld
LDFLAGS=-static -m elf_x86_64 --oformat binary -b elf64-x86-64 -e start -Ttext $(SECTION_TEXT_START_OFFSET) -Tdata $(SECTION_DATA_START_OFFSET)

QUIET = @

.PHONY=clean all image 

all: scrn_clear clean image

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(OBJ)
	@echo 'Linkeando kernel...'
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo ''

	@echo 'Generando imagen del kernel...'
	mv kernel.bin kernel.bin.tmp
	dd if=/dev/zero of=kernel.bin bs=1 count=$(KERNELSIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	dd if=kernel.bin.tmp of=kernel.bin bs=1 count=$(CODESIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	rm kernel.bin.tmp
	@echo ''	

image: kernel.bin $(DEPEND) $(DISK_IMG)
	@echo 'Copiando el $(KERNEL_BIN) a la imagen de diskette'
	$(MCOPY) -o -i $(DISK_IMG) $(KERNEL_BIN) ::/	
	@echo ''
	@echo 'Corriendo bochs'
	./bochs/bin/bochs -q -f bochsrc

$(DISK_IMG): $(DISK_IMG_BZ)
	bzip2 -dk $(DISK_IMG_BZ)

scrn_clear: 
	reset

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	rm -f *.tmp
	rm -f $(DISK_IMG)
	@echo ''

