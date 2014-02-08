
OBJ=$(KERNEL_OBJ) gdt.o mmu.o idt.o isr.o contextManager.o utils.o kmain64.o \
	i386.o


LDFLAGS=-static -m elf_i386 --oformat binary -b elf32-i386

CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include

all: clean ap_startup_code
%.o: %.asm
	nasm -felf32 -o $@ $^

ap_startup_code: ap_startup_code.o
	ld $(LDFLAGS) -Ttext 0x2000 -o $@ $^
	cp $@ ../grub-init/$@

ap_full_code.bin:

clean:
	rm -rf ap_startup_code ap_startup_code.o
