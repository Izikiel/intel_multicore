#!/bin/bash

# El proposito de este script es instalar bochs en la carpeta actual, con
# soporte para SMP y demas chiches. Lo hace automaticamente para evitar 
# problemas de versiones.

if [[ -f ./bochs/bin/bochs ]]; then
	echo "Bochs ya esta instalado"
	exit 0
fi

URL="http://downloads.sourceforge.net/project/bochs/bochs/2.6.2/bochs-2.6.2.tar.gz"
#Prerequisito para bochs, tener gtk
dpkg -l | grep -qw libgtk2.0-dev || apt-get install libgtk2.0-dev

dpkg -l | grep -qw libgtk2.0-dev || sudo apt-get libgtk2.0-dev
wget -O bochs.tar.gz $URL
mkdir bochs-installation
mkdir bochs
tar zxvf bochs.tar.gz -C bochs-installation
cd bochs-installation/bochs-2.6.2
export LDFLAGS=-lpthread
./configure --enable-x86-64 --enable-smp --enable-long-phy-address \
	--enable-debugger --enable-disasm\
	--enable-readline --enable-cpu-level=6\
	--enable-all-optimizations --prefix="$(pwd)/../../bochs"\
	&& make -j 3 && make install 
cd ../..
rm -rf bochs.tar.gz bochs-installation
