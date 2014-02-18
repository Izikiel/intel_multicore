#include "defines.h"
#include "types.h"
#include "sort_code.h"

void sort_ap()
{
    char *start = (char *) start_address;
    char *start_merge = (char *) start_merge_address;
    char *done = (char *) done_address;
    char *finish_copy = (char *) finish_copy_address;
    char *sleep = (char *) sleep_address;
    char *start_copy = (char *) start_copy_address;

    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint32_t *ap_temp = (uint32_t *) ap_temp_address;

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