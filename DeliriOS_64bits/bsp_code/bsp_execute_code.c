#include "bsp_execute_code.h"

static void clean_flags()
{
    *((char *) start_address) = 0;
    *((char *) start_merge_address) = 0;
    *((char *) done_address) = 0;
    *((char *) finish_copy_address) = 0;
    *((char *) start_copy_address) = 0;
}

void sort_bsp()
{
    //si empieza a reventar con GP por el AP,
    //hay que cambiar el origen de linkeo pq este modulo
    //cambio el lugar de origen por su tamaño

    //synchronization flags
    char *start = (char *) start_address;
    char *start_merge = (char *) start_merge_address;
    char *done = (char *) done_address;
    char *finish_copy = (char *) finish_copy_address;
    char *start_copy = (char *) start_copy_address;

    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint32_t *bsp_temp = (uint32_t *) bsp_temp_address;

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