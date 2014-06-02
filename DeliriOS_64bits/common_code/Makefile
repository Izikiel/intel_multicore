CC=gcc
CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include \
  -mno-sse -mno-sse2 -mno-sse3 -mno-sse4.1 -mno-sse4.2

SORT_FLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O3 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include \
  -mno-sse -mno-sse2 -mno-sse3 -mno-sse4.1 -mno-sse4.2
NASM=nasm
NASMFLAGS=-felf64 -I../include/

sort_code.o: sort_code.c
	$(CC) $(SORT_FLAGS) -c -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

OBJ= multicore_common.o gdt.o mmu.o contextManager.o sort_code.o screen_utils.o init_ioapic.o

all: clean $(OBJ)

clean:
	rm -f *.o