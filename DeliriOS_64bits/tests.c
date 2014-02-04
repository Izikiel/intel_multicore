#include "types.h"
#include "tiempo.h"

uint64_t start, stop;

void test_1_core(){
	uint32_t len = 2;

	for (int i = 0; i < 21; ++i, len *= 2)
	{
		MEDIR_TIEMPO_START(start);
		mergesort(test_data[i], len);
		MEDIR_TIEMPO_STOP(stop);

		// ver q tiene silvio para hacer esto print(stop-start);


	}
}

void test_2_cores(){
	len = 2;
	half_len = 1;

	for (int i = 0; i < 21; ++i,
						len *= 2,
						half_len *= 2)
	{
		full_array = test_data[i];

		MEDIR_TIEMPO_START(start);

			mergesort_core1();
			// mergesort_core2 ya tiene q estar corriendo en el ap

		MEDIR_TIEMPO_STOP(stop);

		//print(stop-start);

	}

	finish = 1;
}