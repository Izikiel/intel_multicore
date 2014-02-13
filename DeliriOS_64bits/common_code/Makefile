DEPEND = Makefile\
	../include/contextManager.h ../include/defines.h ../include/gdt.h ../include/idt.h\
	../include/isr.h ../include/mmu.h ../include/types.h \
	../include/irq.h ../include/i386.h

CC=gcc
CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include
NASM=nasm
NASMFLAGS=-felf64 -I../include/

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

OBJ= gdt.o mmu.o idt.o isr.o contextManager.o sort_code.o

all: $(OBJ)

clean:
	rm -f *.o