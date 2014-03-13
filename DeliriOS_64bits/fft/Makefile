CC=gcc
CFLAGS=-std=c99 -m64 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding -I../include


NASM=nasm
NASMFLAGS=-felf64 -I../include/

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

OBJ= fft.o sin.o complex.o

all: $(OBJ)

clean:
	rm -f *.o