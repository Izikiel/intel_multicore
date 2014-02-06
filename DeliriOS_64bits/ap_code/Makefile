LDFLAGS=-static -m elf_i386 --oformat binary -b elf32-i386

all: clean ap_startup_code

%.o: %.asm
	nasm -felf32 -o $@ $^

ap_startup_code: ap_startup_code.o
	ld $(LDFLAGS) -Ttext 0x2000 -o $@ $^
	cp $@ ../grub-init/$@

clean:
	rm -rf ap_startup_code ap_startup_code.o
