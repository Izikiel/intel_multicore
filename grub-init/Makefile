CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld

CFLAGS=-m32 -std=c99 -Werror -Wall -O0\
	-Wno-unused-parameter -Wno-override-init\
	-Wunreachable-code -Wshadow -Wcast-qual \
	-Wformat=2 -Wwrite-strings -Wstrict-prototypes\
 	-Wredundant-decls -Wnested-externs -Wmissing-include-dirs\
	-Wjump-misses-init -Wlogical-op\
	-nostdlib -fno-builtin -nostartfiles\
	-nodefaultlibs -fno-stack-protector -I./include

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

%.o: %.c
	@echo 'Compilando Codigo C...'
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@echo ''

%.o: %.asm
	@echo 'Compilando Codigo Assembler...'
	$(ASM) $(NASMFLAGS) -o $@ $^
	@echo ''

kernel.bin: $(COBJS) $(ASMOBJS)
	@echo 'Linkeando Kernel...'
	$(LINKER) $(LINKERFLAGS) -o $@ $^
	@echo ''

all: clearScr clean kernel.bin
	@echo 'Creando imagen de kernel...'
	./make_kernel_binary.sh
	@echo ''

run-bochs: all
	@echo 'Instalando y corriendo bochs...'
	./install_bochs.sh
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

run-qemu: all
	qemu -smp 2 -cpu pentium -fda floppy.img

clean: clearScr
	@echo 'Limpiando todo...'
	rm -rf $(ASMOBJS) $(COBJS) $(CDEPS) kernel.bin floppy.img bochs.tar.gz
	@echo ''

clearScr:
	clear

.DEFAULT_GOAL := all
