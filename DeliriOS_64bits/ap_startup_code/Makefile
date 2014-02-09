LDFLAGS_start=-static -m elf_i386 --oformat binary -b elf32-i386

all: clean ap_startup_code
ap_startup_code.o: ap_startup_code.asm
	nasm -felf32 -o $@ $^

ap_startup_code: ap_startup_code.o
	ld $(LDFLAGS_start) -Ttext 0x2000 -o $@ $^
	cp $@ ../grub-init/$@

clean:
	rm -rf ap_startup_code *.o