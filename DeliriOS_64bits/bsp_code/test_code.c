#include "types.h"
#include "tiempo.h"
#include "defines.h"
#include "sort_code.h"
#include "bsp_execute_code.h"
#include "screen_utils.h"

#define max_len (8*1024*1024)

uint64_t start, stop;

void wait(){
	uint64_t limit = 53200000000;
	MEDIR_TIEMPO_START(start);
	do{
		MEDIR_TIEMPO_STOP(stop);
	}
	while((stop - start) < limit);
	return;
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

void test_1_core(){
	// clear_screen();
	// uint8_t n_cores = *((uint8_t*) number_of_cores_address);

	// print_string("Number of cores: ",0,0);

	// print_number_u64(n_cores, 0, 18);

	// wait();
	// wait();
	// wait();
	// wait();
	clean_array(max_len);
	clear_screen();
	uint32_t* len = (uint32_t*) array_len_address;
	uint32_t* array = (uint32_t*) array_start_address;
	uint8_t line = 0;

	uint8_t col = 0;
	print_string("sort 1 core", line++, col);
	for (*len = 2; *len < max_len; *len *= 2){
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		heapsort(array, *len);
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			print_number_u64(stop-start, line++, col);
		}
		else{
			print_string("bad_sort :(", line++, col);
		}
	}
	print_string("Done! :D", ++line, col);

	//breakpoint
}

void test_2_cores(){
	uint32_t* len = (uint32_t*) array_len_address;
	char* sleep = (char*) sleep_address;
	uint8_t col = 34;
	uint8_t line = 0;

	print_string("sort 2 cores", line++, col);

	for (*len = 2; *len < max_len; *len *= 2){
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		sort_bsp();
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			print_number_u64(stop-start, line++, col);
		}
		else{
			print_string("bad_sort :(", line++, col);
		}

	}
	print_string("Done! :D", ++line, col);
	*sleep = 1;
}

void sum_vector(uint32_t len){
	uint32_t *array = (uint32_t *) array_start_address;
	for (uint32_t i = 0; i < len; ++i)
		array[i]++;
}

void test_sum_vector1(){
	uint32_t* len = (uint32_t*) array_len_address;
	uint8_t col = 68;
	uint8_t line = 0;

	print_string("sum 1 core", line++, col);
	for (*len = 2; *len < max_len; *len *= 2){
		MEDIR_TIEMPO_START(start);
		sum_vector(*len);
		MEDIR_TIEMPO_STOP(stop);
		print_number_u64(stop-start, line++, col);
	}
	print_string("Done! :D", ++line, col);
}

void test_sum_vector2(){
	uint32_t* len = (uint32_t*) array_len_address;
	char* sleep = (char*) sleep_address;
	uint8_t col = 102;
	uint8_t line = 0;

	print_string("sum 2 cores", line++, col);

	for (*len = 2; *len < max_len; *len *= 2){
		MEDIR_TIEMPO_START(start);
		sum_vector_bsp();
		MEDIR_TIEMPO_STOP(stop);
		print_number_u64(stop-start, line++, col);
	}
	print_string("Done! :D", ++line, col);
	*sleep = 1;
}

void test_ipi_cores(){
	clear_screen();
	uint32_t* len = (uint32_t*) array_len_address;
	char* sleep = (char*) sleep_address;
	uint8_t col = 0;
	uint8_t line = 0;

	print_string("sort 2 cores ipis", line++, col);

	for (*len = 2; *len < max_len; *len *= 2){
		uint32_t seed = 13214;
		generate_global_array(seed, *len);
		MEDIR_TIEMPO_START(start);
		sort_bsp_ipi();
		MEDIR_TIEMPO_STOP(stop);
		if(verfiy_sort()){
			print_number_u64(stop-start, line++, col);
		}
		else{
			print_string("bad_sort :(", line++, col);
		}

	}
	print_string("Done! :D", ++line, col);
	*sleep = 1;
}