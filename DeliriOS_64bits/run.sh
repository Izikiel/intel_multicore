#!/bin/bash

if [[ $1 == 'r' ]]; then
	pushd ./grub-init/
	make run-bochs
	popd
fi
if [[ $1 == 'c' ]]; then
	pushd ./grub-init/
	make
	popd
fi