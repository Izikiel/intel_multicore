CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld

CFLAGS=-std=c99 -m32 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I./include

NASMFLAGS=-i./include/ -f elf32
LINKSCRIPT=build/linker_script.ld
LINKERFLAGS=-static -melf_i386 -T $(LINKSCRIPT)

.PHONY=all clean

CSOURCES=$(wildcard src/**/*.c src/*.c)
CHEADERS=$(wildcard include/**/*.h include/*.h)
ASMSOURCES=$(wildcard src/**/*.asm src/*.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))
CDEPS=$(patsubst %.o,%.d,$(COBJS))

BOCHSDIR?=./bochs/bin
BOCHSCONF?=run/bochsrc.txt
BOCHS_SCRIPT?=run/bochsdbg.txt

%.o: %.c
	@echo 'Compilando Codigo C...'
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@echo ''

%.o: %.asm
	@echo 'Compilando Codigo Assembler...'
	$(ASM) $(NASMFLAGS) -o $@ $^
	@echo ''

bootstrap32.elf32: $(COBJS) $(ASMOBJS)
	@echo 'Linkeando Kernel...'
	$(LINKER) $(LINKERFLAGS) -o $@ $^
	@echo ''

all: clearScr clean bootstrap32.elf32
	@echo 'Creando imagen de kernel...'
	./make_kernel_binary.sh
	@echo ''

run-bochs: all
	@echo 'Instalando y corriendo bochs...'
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF) -rc $(BOCHS_SCRIPT)

run-qemu: all
	qemu -smp 2 -cpu pentium -fda floppy.img

clean: clearScr
	@echo 'Limpiando todo...'
	rm -rf $(ASMOBJS) $(COBJS) $(CDEPS) bootstrap32.elf32 floppy.img bochs.tar.gz
	@echo ''

clearScr:
	clear

.DEFAULT_GOAL := all
