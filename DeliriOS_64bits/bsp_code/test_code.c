#include "types.h"
#include "tiempo.h"
#include "defines.h"
#include "sort_code.h"
#include "bsp_execute_code.h"
#include "screen_utils.h"

#define max_len (1024*1024)

uint64_t start, stop;

void clean_variables(){
	uint32_t* start_memory_area = (uint32_t*) static_variable_area;
	for (int i = 0; i < 1024; ++i)
		start_memory_area[i] = 0;
}

uint32_t rand(){
	uint64_t seed = *((uint64_t*) seed_address);
	seed = seed * 1103515245 + 12345;
	*((uint64_t*) seed_address) = seed;

	return (uint32_t) (seed / 65536) % 32768;
}

void generate_global_array(uint64_t seed, uint32_t len){
	*((uint64_t*) seed_address) = seed;
	uint32_t* array = (uint32_t*) array_start_address;
	for (uint32_t i = 0; i < len; ++i)
	{
		array[i] = rand();
	}
}

bool verfiy_sort(){
	//breakpoint
	uint32_t* array = (uint32_t*) array_start_address;
	uint32_t len = *((uint32_t*) array_len_address);
	for (int i = 1; i < len; ++i){
		if (array[i-1] > array[i]){
			return false;
		}
	}
	return true;
}

void clean_array(uint32_t len){
	uint32_t* array = (uint32_t*) array_start_address;
	for (uint32_t i = 0; i < len; ++i)
		array[i] = 0;
}

bool verfiy_zero(uint32_t len){
	uint32_t* array = (uint32_t*) array_start_address;
	for (uint32_t i = 0; i < len; ++i)
		if (array[i] != 0){
			return false;
		}
	return true;
}

void test_1_core(){
	clean_variables();
	clear_screen();
	clean_array(max_len);
	uint32_t* len = (uint32_t*) array_len_address;
	uint32_t* array = (uint32_t*) array_start_address;
	uint8_t line = 1;
	if (verfiy_zero(max_len)){
		print_string("Todo en cero! :D",0);
	}
	else{
		print_string("algo no esta en cero :(", 0);
	}
	for (*len = 2; *len < max_len; *len *= 2){
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		//breakpoint
		heapsort(array, *len);
		//breakpoint
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			print_number_u64(stop-start, line);
		}
		else{
			print_string("bad_sort :(", line);
		}
		line++;
	}
	print_string("Listo el sorteo :O", line);
	breakpoint
		// ver q tiene silvio para hacer esto print(stop-start);
}

void test_2_cores(){
	clear_screen();
	clean_variables();
	uint32_t* len = (uint32_t*) array_len_address;
	char* sleep = (char*) sleep_address;

	print_string("Sorteando con 2 cores! :D", 0);

	uint8_t line = 1;
	for (*len = 2; *len < max_len; *len *= 2)
	{
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		sort_bsp();
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			print_number_u64(stop-start, line);
		}
		else{
			print_string("bad_sort :(", line);
		}
		line++;

	}
	print_string("Listo el sorteo :O", line);
	*sleep = 1;


}