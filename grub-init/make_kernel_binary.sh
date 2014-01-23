#!/bin/bash

# Este script se utiliza para pasar el binario del kernel a una imagen de 
# bochs, incluyendo ademas todos los modulos.

cp floppy_raw.img floppy.img
 
e2cp kernel.bin floppy.img:/
e2cp build/menu.lst floppy.img:/boot/grub/menu.lst
