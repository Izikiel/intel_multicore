#include "types.h"
#include "tiempo.h"
#include "defines.h"
#include "sort_code.h"
#include "bsp_execute_code.h"

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
	for (int i = 1; i < len; ++i)
	{
		if (array[i-1] > array[i]){
			//breakpoint
			__asm __volatile("mov %0, %%eax": :"r" (i));
			return false;
		}
	}
	return true;
}

void clean_array(uint32_t len){
	char* array = (char*) array_start_address;
	for (uint32_t i = 0; i < len; ++i)
		array[i] = 0;
}

void test_1_core(){
	clean_variables();
	clean_array(max_len);
	uint32_t* len = (uint32_t*) array_len_address;
	uint32_t* array = (uint32_t*) array_start_address;
	for (*len = 2; *len < max_len; *len *= 2)
	{
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		//MEDIR_TIEMPO_START(start);
		//breakpoint
		heapsort(array, *len);
		//breakpoint
		//MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
		}
		// else{
		// 	breakpoint
		// 	breakpoint
		// }
		clean_array(*len);
	}
	breakpoint
		// ver q tiene silvio para hacer esto print(stop-start);
}

void test_2_cores(){
	breakpoint
	breakpoint
	clean_variables();
	//uint64_t max_len = 10 * 1024 * 1024;
	uint32_t* len = (uint32_t*) array_len_address;
	breakpoint
	breakpoint

	for (*len = 2; *len < max_len; *len *= 2)
	{
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		sort_bsp();
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			//breakpoint
			//breakpoint
		}
		else{
			//breakpoint
			//__asm __volatile("nop": :);
		}

	}

	//sfinish = 1;
}