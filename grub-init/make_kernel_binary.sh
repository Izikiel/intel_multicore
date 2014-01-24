#!/bin/bash
#preparo la imagen de grub virgen
cp floppy_raw.img floppy.img

#copio el loader de 32 bits
e2cp bootstrap32.elf32 floppy.img:/
#recompilo el OS y lo copio al floppy
@echo 'Recompilando DeliriOS...'
pushd ../DeliriOS_64bits
make
popd
e2cp kernel64.bin64 floppy.img:/
#copio el menu de grub
e2cp build/menu.lst floppy.img:/boot/grub/menu.lst
