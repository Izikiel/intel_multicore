#!/bin/bash

if [[ $1 == 'r' ]]; then
	pushd ./grub-init/
	make run-bochs
	popd
elif [[ $1 == 'c' ]]; then
	pushd ./grub-init/
	make
	popd
else
	echo 'r : compile + run'
	echo 'c : compile'
fi
