#!/bin/bash

if [[ $1 == 'r' ]]; then
	pushd ./grub-init/
	make run-bochs
	popd
elif [[ $1 == 'c' ]]; then
	pushd ./grub-init/
	make
	popd
elif [[ $1 == 'usb' ]]; then
	pushd ./grub-init/
	./build_for_usb.sh $2
	popd
else
	echo 'r : compile + run'
	echo 'c : compile'
	echo 'usb path: compile files and copies them to path and path/boot/grub/'
	echo '			for this option you need an usb with grub 0.97 installed'
fi
