CC=gcc
ASM=nasm
LINKER=ld
LINK.o=ld

CFLAGS=-m32 -O2 -std=c11 -Werror -Wall -Wextra\
	-Wno-unused-parameter -Wno-override-init\
	-Wunreachable-code -Wshadow -Wcast-qual \
	-nostdlib -fno-builtin -nostartfiles\
	-nodefaultlibs -fno-stack-protector -I./include
NASMFLAGS=-i./include/ -f elf32
LINKSCRIPT=build/linker_script.ld
LINKERFLAGS=-melf_i386 -T $(LINKSCRIPT)

.PHONY=all clean

CSOURCES=$(wildcard src/**/*.c src/*.c)
CHEADERS=$(wildcard include/**/*.h include/*.h)
ASMSOURCES=$(wildcard src/**/*.asm src/*.asm)
ASMOBJS=$(patsubst %.asm,%.o,$(ASMSOURCES))
COBJS=$(patsubst %.c,%.o,$(CSOURCES))
CDEPS=$(patsubst %.o,%.d,$(COBJS))

BOCHSDIR?=./bochs/bin
BOCHSCONF?=run/bochsrc.txt

-include $(CDEPS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	$(CC) -MM $(CFLAGS) $*.c > $*.d
	@mv -f $*.d $*.d.tmp
	@sed -e 's|.*:|$*.o:|' < $*.d.tmp > $*.d
	@sed -e 's/.*://' -e 's/\\$$//' < $*.d.tmp | fmt -1 | \
		sed -e 's/^ *//' -e 's/$$/:/' >> $*.d
	@rm -f $*.d.tmp

%.o: %.asm
	$(ASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(COBJS) $(ASMOBJS)
	$(LINKER) $(LINKERFLAGS) -o $@ $^

all: kernel.bin Makefile build/linker_script.ld
	./make_kernel_binary.sh

run-bochs: all
	./install_bochs.sh
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

run-qemu: all
	qemu -smp 2 -cpu pentium -fda floppy.img

clean:
	cd build/grub_modules_build && make clean
	rm -rf $(ASMOBJS) $(COBJS) $(CDEPS)\
	   	kernel.bin floppy.img bochs.tar.gz

zipped: clean
	tar zcvf entrega.tar.gz\
		./Makefile\
		./floppy_raw.img\
		./run\
		./build\
		./src\
		./test\
		./include

types: types.vim
types.vim: $(CSOURCES) $(CHEADERS)
	ctags --c-kinds=gstu -o- $^ |\
		awk 'BEGIN{printf("syntax keyword Type\t")}\
			{printf("%s ", $$1)}END{print ""}' > $@
