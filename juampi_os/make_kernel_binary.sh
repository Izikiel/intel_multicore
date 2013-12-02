#!/bin/bash

# Este script se utiliza para pasar el binario del kernel a una imagen de 
# bochs, incluyendo ademas todos los modulos.

# Los modulos son todos los archivos (con el mismo nombre) puestos en la 
# carpeta /build/grub_modules

# Son accesibles entonces como modulos con el mismo nombre que el nombre de
# base del archivo

cp floppy_raw.img floppy.img
 
#Copiar todos los modulos de grub al floppy y marcarlos como modulos en el
#menu de GRUB para que los detecte

FILE_LISTING=""
for file in build/grub_modules/*
do
        bn=$(basename $file)
        e2cp $file floppy.img:/$bn
        FILE_LISTING=$FILE_LISTING$'\n'"module /$bn"
done
 
#Sacar lineas en blanco al final y escapearlo para sed
FILE_LISTING=$(echo "$FILE_LISTING" | sed -e 's/[\/&]/\\&/g' | grep -v '^$')
#Reemplazar la lista de modules en el menu para que grub los tenga
sed -e "s/{{modules}}/$FILE_LISTING/g" < build/menu_template.lst > build/menu.lst
 
e2cp kernel.bin floppy.img:/
e2cp build/menu.lst floppy.img:/boot/grub/menu.lst
