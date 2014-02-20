#include "bsp_execute_code.h"

static void clean_flags()
{
    *((uint8_t *) start_address) = 0;
    *((uint8_t *) start_merge_address) = 0;
    *((uint8_t *) done_address) = 0;
    *((uint8_t *) finish_copy_address) = 0;
    *((uint8_t *) start_copy_address) = 0;
}

void sort_bsp()
{
    //si empieza a reventar con GP por el AP,
    //hay que cambiar el origen de linkeo pq este modulo
    //cambio el lugar de origen por su tamaño

    //synchronization flags
    uint8_t *start = (uint8_t *) start_address;
    uint8_t *start_merge = (uint8_t *) start_merge_address;
    uint8_t *done = (uint8_t *) done_address;
    uint8_t *finish_copy = (uint8_t *) finish_copy_address;
    uint8_t *start_copy = (uint8_t *) start_copy_address;

    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint32_t *bsp_temp = (uint32_t *) temp_address;

    //ready, set, go!
    *start = 1;
    heapsort(array, *len/2);

    active_wait(*done);
    *done = 0;
    *start_merge = 1;

    limit_merge(array, bsp_temp, 0, (*len / 2) - 1, *len - 1, *len / 2);

    active_wait(*done);
    *done = 0;
    *start_copy = 1;

    copy(array, 0, bsp_temp, 0, *len / 2);
    active_wait(*finish_copy);

    clean_flags();

}

void sum_vector_bsp(){
    uint8_t *start = (uint8_t *) start_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *finish = (uint8_t *) finish_copy_address;

    clean_flags();
    *start = 1;

    for (uint32_t i = 0; i < *len/2; ++i) {
        array[i]++;
    }

    active_wait(*finish);
    clean_flags();
}