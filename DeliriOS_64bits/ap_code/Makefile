DEPEND = Makefile\
	ap_full_code.asm ../common_code/gdt.c ../common_code/isr.asm \
	../common_code/mmu.c ../common_code/contextManager.c \
	../include/contextManager.h ../include/defines.h ../include/gdt.h ../include/idt.h \
	../include/isr.h ../include/mmu.h ../include/ports.h  ../include/types.h \
	../include/irq.h ../include/i386.h ../include/timer.h

KERNEL_SRC=ap_full_code.asm
KERNEL_OBJ=ap_full_code.o


OBJ=$(KERNEL_OBJ) ../common_code/gdt.o ../common_code/ports.o \
	../common_code/mmu.o ../common_code/idt.o \
	../common_code/isr.o ../common_code/contextManager.o \
	../common_code/i386.o ../common_code/timer.o

LD=ld
LDFLAGS_full=-static -melf_x86_64 

CC=gcc
CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include

NASM=nasm
NASMFLAGS=-felf64 -I../include/


QUIET = @

.PHONY=clean all image


all: clean common_code ap_full_code.bin $(DEPEND)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

common_code:
	cd ../common_code; make

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

ap_full_code.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

clean:
	rm -rf *.o
	rm -rf *.bin
	cd ../common_code; make clean


