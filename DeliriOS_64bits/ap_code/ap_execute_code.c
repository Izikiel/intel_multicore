#include "defines.h"
#include "types.h"
#include "sort_code.h"

void sort_ap()
{
    uint8_t *start = (uint8_t *) start_address;
    uint8_t *start_merge = (uint8_t *) start_merge_address;
    uint8_t *done = (uint8_t *) done_address;
    uint8_t *finish_copy = (uint8_t *) finish_copy_address;
    uint8_t *sleep = (uint8_t *) sleep_address;
    uint8_t *start_copy = (uint8_t *) start_copy_address;

    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint32_t *ap_temp = (uint32_t *) (temp_address + TEN_MEGA);

    //waiting for go!

    active_wait(*start) {
        if (*sleep) {
            return;
        }
    }

    *start = 0;
    heapsort(array + *len / 2, *len / 2);
    *done = 1;

    active_wait(*start_merge);

    limit_merge_reverse(array, ap_temp, 0, (*len / 2) - 1, *len - 1, *len / 2);
    *done = 1;

    active_wait(*start_copy);

    copy(array, *len / 2, ap_temp, 0, *len / 2);
    *finish_copy = 1;

}

void sum_vector_ap(){
    uint8_t *start = (uint8_t *) start_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *sleep = (uint8_t *) sleep_address;


    active_wait(*start) {
        if (*sleep) {
            return;
        }
    }

    for (uint32_t i = *len/2; i < *len; ++i) {
        array[i]++;
    }

    *((uint8_t *) finish_copy_address) = 1;
}