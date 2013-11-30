CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld

CFLAGS=-m32 -O2 -std=c99 -Wall -nostdlib -fno-builtin -nostartfiles -nodefaultlibs -fno-stack-protector -I./include
NASMFLAGS=-i./include/ -f elf32
LINKSCRIPT=build/linker_script.ld
LINKERFLAGS=-melf_i386 -T $(LINKSCRIPT)

.PHONY=all clean

CSOURCES=$(wildcard src/**/*.c src/*.c)
ASMSOURCES=$(wildcard src/**/*.asm src/*.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))

BOCHSDIR?=~/bochs/bin
BOCHSCONF?=run/bochsrc.txt

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(ASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(COBJS) $(ASMOBJS)
	$(LINKER) $(LINKERFLAGS) -o $@ $^

all: kernel.bin Makefile
	./make_kernel_binary.sh

run: all
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

clean:
	rm -rf $(ASMOBJS) $(COBJS) kernel.bin floppy.img

zipped: clean
	tar zcvf entrega.tar.gz\
		./Makefile\
		./floppy_raw.img\
		./run\
		./build\
		./src\
		./test\
		./include
