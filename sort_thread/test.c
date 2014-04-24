#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "stdbool.h"
#include "pthread.h"
#include "aux.h"
#include "tiempo.h"

#ifndef N
#define N (8*1024*1024)
#endif

int array[N];
int bsp_temp[N];
int ap_temp[N];
int buffer[N];
uint64_t global_seed;
uint64_t sync_times[3];


void do_sort_pthreads(uint32_t len);
void *thread_sort(void *v_len);
void *thread_merge(void *v_len);
void *thread_copy(void *v_len);
void exit_if_error(int err_code);
void test_pthreads();
uint32_t custom_rand();
void generate_global_array(uint64_t seed, uint32_t len);
bool verfiy_sort(uint32_t len);
void test_pthreads_sync();



int main()
{
    test_pthreads();
    test_pthreads_sync();
    return 0;
}


void do_sort_pthreads(uint32_t len)
{
    void *status;
    pthread_t ap_heap_sort, ap_merge, ap_copy;
    pthread_attr_t join_attr;

    pthread_attr_init(&join_attr);
    pthread_attr_setdetachstate(&join_attr, PTHREAD_CREATE_JOINABLE);

    // SORT
    exit_if_error(
        pthread_create(&ap_heap_sort, &join_attr, thread_sort, (void *) &len)
    );

    heapsort(array, len / 2);

    exit_if_error(
        pthread_join(ap_heap_sort, &status)
    );

    //  MERGE
    exit_if_error(
        pthread_create(&ap_merge, &join_attr, thread_merge, (void *) &len)
    );

    limit_merge(array, bsp_temp, 0, (len / 2) - 1, len - 1, len / 2);

    exit_if_error(
        pthread_join(ap_merge, &status)
    );

    //  COPY
    exit_if_error(
        pthread_create(&ap_copy, &join_attr, thread_copy, (void *) &len)
    );

    copy(array, 0, bsp_temp, 0, len / 2);

    exit_if_error(
        pthread_join(ap_copy, &status)
    );

    pthread_attr_destroy(&join_attr);
}

void *dummy_function(void *p)
{
    pthread_exit(NULL);
}

void sort_measure_sync(uint32_t len)
{
    void *status;
    pthread_t dummy;
    pthread_attr_t join_attr;
    uint64_t start, stop;

    pthread_attr_init(&join_attr);
    pthread_attr_setdetachstate(&join_attr, PTHREAD_CREATE_JOINABLE);

    // SORT
    exit_if_error(
        pthread_create(&dummy, &join_attr, dummy_function, (void *) NULL)
    );

    heapsort(array, len / 2);
    heapsort(array + len / 2, len / 2);

    MEDIR_TIEMPO_START(start);
    exit_if_error(
        pthread_join(dummy, &status)
    );
    MEDIR_TIEMPO_STOP(stop);
    sync_times[0] = stop - start;

    //  MERGE
    exit_if_error(
        pthread_create(&dummy, &join_attr, dummy_function, (void *) NULL)
    );

    limit_merge(array, bsp_temp, 0, (len / 2) - 1, len - 1, len / 2);
    limit_merge_reverse(array, ap_temp, 0, (len / 2) - 1, len - 1, len / 2);

    MEDIR_TIEMPO_START(start);
    exit_if_error(
        pthread_join(dummy, &status)
    );
    MEDIR_TIEMPO_STOP(stop);
    sync_times[1] = stop - start;

    //  COPY
    exit_if_error(
        pthread_create(&dummy, &join_attr, dummy_function, (void *) NULL)
    );

    copy(array, 0, bsp_temp, 0, len / 2);
    copy(array, len / 2, ap_temp, 0, len / 2);

    MEDIR_TIEMPO_START(start);
    exit_if_error(
        pthread_join(dummy, &status)
    );
    MEDIR_TIEMPO_STOP(stop);
    sync_times[2] = stop - start;

    pthread_attr_destroy(&join_attr);
}

void *thread_sort(void *v_len)
{
    uint32_t len = *((uint32_t *) v_len);
    heapsort(array + len / 2, len / 2);
    pthread_exit(NULL);
}

void *thread_merge(void *v_len)
{
    uint32_t len = *((uint32_t *) v_len);
    limit_merge_reverse(array, ap_temp, 0, (len / 2) - 1, len - 1, len / 2);
    pthread_exit(NULL);
}

void *thread_copy(void *v_len)
{
    uint32_t len = *((uint32_t *) v_len);
    copy(array, len / 2, ap_temp, 0, len / 2);
    pthread_exit(NULL);
}

void exit_if_error(int err_code)
{
    if (err_code) {
        printf("ERROR; return code from create or join is %d\n", err_code);
        exit(-1);
    }
}

void test_pthreads()
{
    printf("Testing pthreads\n");
    printf("Elementos\tCiclos\n");

    uint64_t start, stop;

    for (uint32_t len = 2; len < N; len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, len);
        MEDIR_TIEMPO_START(start);
        do_sort_pthreads(len);
        MEDIR_TIEMPO_STOP(stop);
        if (verfiy_sort(len)) {
            printf("%u\t\t%lu \n", len, stop - start);
        } else {
            printf("Bad sort! :(\n");
        }
    }
    printf("Done! :D\n");
}

void test_pthreads_sync()
{
    printf("\nTesting pthreads sync\n");
    printf("Elementos\tCiclos\t\tCiclos\t\tCiclos\n");

    for (uint32_t len = 2; len < N; len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, len);
        sort_measure_sync(len);
        if (verfiy_sort(len)) {
            printf("%u\t\t%lu\t\t%lu\t\t%lu\n", len, sync_times[0], sync_times[1], sync_times[2]);
        } else {
            printf("Bad sort! :(\n");
        }
    }
    printf("Done! :D\n");
}

uint32_t custom_rand()
{
    global_seed = global_seed * 1103515245 + 12345;
    return (uint32_t) (global_seed / 65536) % 32768;
}

void generate_global_array(uint64_t seed, uint32_t len)
{
    global_seed = seed;
    for (uint32_t i = 0; i < len; ++i) {
        array[i] = custom_rand();
    }
}

bool verfiy_sort(uint32_t len)
{
    for (uint32_t i = 1; i < len; ++i) {
        if (array[i - 1] > array[i]) {
            return false;
        }
    }
    return true;
}
