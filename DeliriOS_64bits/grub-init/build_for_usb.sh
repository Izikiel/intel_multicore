#!/bin/bash

#recompilo el OS y lo copio al usb
make bootstrap32.elf32
@echo 'Recompilando DeliriOS...'
pushd ../bsp_code
make clean
make
popd
pushd ../ap_code
make clean
make
popd
pushd ../ap_startup_code
make clean
make
popd

echo 'Copiando archivos al usb'
cp bootstrap32.elf32 $1/
cp kernel64.bin64 $1/
cp ap_*		$1/
cp ./grub_usb/menu.lst  $1/boot/grub/
echo 'Disfrute de DeliriOS booteable por usb :D'