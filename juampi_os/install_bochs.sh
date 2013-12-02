#!/bin/bash

# El proposito de este script es instalar bochs en la carpeta actual, con
# soporte para SMP y demas chiches. Lo hace automaticamente para evitar 
# problemas de versiones.

if [[ -d bochs ]]; then
	echo "Bochs ya esta instalado"
	exit 0
fi

URL="http://downloads.sourceforge.net/project/bochs/bochs/2.6.2/bochs-2.6.2.tar.gz"

wget -O bochs.tar.gz $URL
mkdir bochs-installation
mkdir bochs
tar zxvf bochs.tar.gz -C bochs-installation
cd bochs-installation/bochs-2.6.2
export LDFLAGS=-lpthread
./configure --enable-smp --enable-debugger --enable-disasm\
	--enable-readline --enable-cpu-level=6\
	--enable-all-optimizations --prefix="$(pwd)/../../bochs"\
	&& make -j 3 && make install 
cd ../..
rm -rf bochs.tar.gz bochs-installation
