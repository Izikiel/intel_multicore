	cc -DOZ_HW_TYPE_486 -I../includes/ -o make_param_block make_param_block.c
	./make_param_block
	mv -f param_block.4096 ../objects/
	rm -f make_param_block
