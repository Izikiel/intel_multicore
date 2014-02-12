#include "defines.h"
#include "types.h"
#include "sort_code.h"

void clean_variables(){
	char* start_memory_area = (char*) static_variable_area;
	uint32_t len = 3 * sizeof(char) + sizeof(uint32_t*) + sizeof(uint32_t);
	for (int i = 0; i < len; ++i)
		start_memory_area[i] = 0;
}

void sort_bsp(){
	char start = *((char*) start_address);
	char start_merge = *((char*) start_merge_address);
	char done = *((char*) done_address);
	uint32_t* array = (uint32_t*) array_start_address;
	uint32_t len = *((uint32_t) array_len_address);
}