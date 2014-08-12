#include "ap_execute_code.h"
void signal_finished();

void sort_ap()
{
    uint64_t *start = (uint64_t *) start_address;
    uint64_t *start_merge = (uint64_t *) start_merge_address;
    uint64_t *done = (uint64_t *) done_address;
    uint64_t *finish_copy = (uint64_t *) finish_copy_address;
    uint64_t *sleep = (uint64_t *) sleep_address;
    uint64_t *start_copy = (uint64_t *) start_copy_address;
    *sleep = 0;
    //waiting for go!
    active_wait(*sleep) {
        active_wait(*start) {
            if (*sleep) {
                return;
            }
        }
        uint32_t *len = (uint32_t *) array_len_address;
        uint32_t *array = (uint32_t *) array_start_address;
        uint32_t *ap_temp = (uint32_t *) (temp_address + TEN_MEGA);

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
    *sleep = 0;
}

void ap_sync()
{
    uint64_t *start = (uint64_t *) start_address;
    uint64_t *done = (uint64_t *) done_address;
    uint64_t *sleep = (uint64_t *) sleep_address;
    // uint64_t *start_merge = (uint64_t *) start_merge_address;
    // uint64_t *finish_copy = (uint64_t *) finish_copy_address;
    // uint64_t *start_copy = (uint64_t *) start_copy_address;

    *sleep = 0;
    *start = 0;
    //waiting for go!
    active_wait(*sleep) {
        active_wait(*start) {
            if (*sleep) {
                return;
            }
        }
        *start = 0;
        *done = 1;
        // active_wait(*start_merge);
        // *done = 1;
        // active_wait(*start_copy);
        // *finish_copy = 1;
    }
    // *sleep = 0;
}



void sum_vector_ap()
{
    uint64_t *start = (uint64_t *) start_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *len = (uint32_t *) array_len_address;
    uint64_t *sleep = (uint64_t *) sleep_address;


    active_wait(*start) {
        if (*sleep) {
            return;
        }
    }

    for (uint32_t i = *len / 2; i < *len; ++i) {
        array[i]++;
    }

    *((uint64_t *) finish_copy_address) = 1;
}


void sort_ap_int()
{
    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    heapsort(array + *len / 2, *len / 2);
    signal_finished();
}


void merge_ap_int()
{
    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *ap_temp = (uint32_t *) (temp_address + TEN_MEGA);

    limit_merge_reverse(array, ap_temp, 0, (*len / 2) - 1, *len - 1, *len / 2);
    signal_finished();
}

void copy_ap_int()
{
    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *ap_temp = (uint32_t *) (temp_address + TEN_MEGA);

    copy(array, *len / 2, ap_temp, 0, *len / 2);
    signal_finished();
}

void ap_jump()  //prueba para ipis
{
    // clear_screen();
    // print_string("Soy un ap que salta", 0, 0);
    signal_finished();
}