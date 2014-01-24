#include "types.h"

void test_1_core(){
	uint32_t len = 2;
	for (int i = 0; i < 21; ++i)
	{
		MEDIR_TIEMPO_START(start);
		mergesort(test_data[i], len);
		MEDIR_TIEMPO_STOP(stop);

		print(stop-start);

		len *= 2;
	}
}

void test_2_cores(){
	uint32_t len = 2;
	for (int i = 0; i < 21; ++i)
	{
		MEDIR_TIEMPO_START(start);
		mergesort(test_data[i], len);
		MEDIR_TIEMPO_STOP(stop);

		print(stop-start);

		len *= 2;
	}
}