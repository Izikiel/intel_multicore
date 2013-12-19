MCOPY=mcopy

KERNEL_SRC=kernel.asm
KERNEL_OBJ=kernel.o
KERNEL_BIN=kernel.bin

DEPEND = Makefile \
	kernel.asm isr.asm \
	imprimir.mac \
	gdt.c gdt.h pic.c pic.h idt.c idt.h isr.h mmu.c mmu.h \
	contextManager.h contextManager.c \
	syscall_wrapper.h utils.h utils.c asmutils.asm\
	syscall_impl.h syscall_impl.c 

OBJ=$(KERNEL_OBJ) gdt.o pic.o idt.o isr.o mmu.o utils.o \
	asmutils.o syscall_impl.o contextManager.o

DISK_IMG=diskette.img
DISK_IMG_BZ=diskette.img.bz2

CFLAGS=-std=c99 -m32 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding

CODESIZE=60928 # = 65536 - 0x1200 = 64KB - 0x1200
KERNELSIZE=60928 # = CODESIZE

CC=gcc
NASM=nasm
NASMFLAGS=-felf32
LD=ld
LDFLAGS=-static -m elf_i386 --oformat binary -b elf32-i386 -e start -Ttext 0x1200

QUIET = @

.PHONY=clean all image 

all: genbin image

genbin: genbin.c
	$(CC) -std=c99 -m32 -g -ggdb -Wall -Werror -o genbin genbin.c

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

$(DISK_IMG): $(DISK_IMG_BZ)
	bzip2 -dk $(DISK_IMG_BZ)

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	rm -f *.tmp
	rm -f $(DISK_IMG)
	rm -f genbin
	@echo ''

