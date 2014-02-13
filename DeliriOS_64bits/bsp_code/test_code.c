#include "types.h"
#include "tiempo.h"
#include "defines.h"


uint64_t start, stop;

void clean_variables(){
	char* start_memory_area = (char*) static_variable_area;
	for (int i = 0; i < 1024; ++i)
		start_memory_area[i] = 0;
}

uint32_t rand(){
	uint32_t* seed = (uint32_t*) seed_addres;
	*seed = (*seed) * 1103515245 +12345;
	return (*seed / 65536) % 32768;
}

void generate_global_array(uint32_t seed, uint32_t len){
	*((uint32_t*) seed_addres) = seed;
	uint32_t* array = (uint32_t*) array_start_address;
	for (int i = 0; i < len; ++i)
	{
		array[i] = rand();
	}
}

void clean_flags(){
	*((char*) start_address) = 0;
	*((char*) start_merge_address) = 0;
	*((char*) done_address) = 0;
}

bool verfiy_sort(){
	uint32_t* array = (uint32_t*) array_start_address;
	for (int i = 1; i < *((uint64_t*) array_len_address); ++i)
	{
		if (array[i] < array[i-1])
			return false;
	}
	return true;
}

void test_1_core(){
	uint64_t max_len = 10 * 1024 * 1024;
	uint64_t* len = (uint64_t*) array_len_address;
	uint32_t* array = (uint32_t*) array_start_address;
	for (*len = 2; *len < max_len; *len *= 2)
	{
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		heapsort(array, *len);
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			// print ok!
		}
		else{
			// print bad :(
		}
	}
		// ver q tiene silvio para hacer esto print(stop-start);
}

void test_2_cores(){
	uint64_t max_len = 10 * 1024 * 1024;
	uint64_t* len = (uint64_t*) array_len_address;
	uint32_t* array = (uint32_t*) array_start_address;
	clean_variables();

	for (*len = 2; *len < max_len; *len *= 2)
	{
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		sort_bsp();
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			// print ok!
		}
		else{
			// print bad :(
		}

	}

	finish = 1;
}