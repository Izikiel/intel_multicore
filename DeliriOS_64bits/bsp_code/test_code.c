#include "types.h"
#include "tiempo.h"
#include "defines.h"
#include "sort_code.h"
#include "bsp_execute_code.h"
#include "screen_utils.h"

#include "fft.h"
#include "complex.h"

#define max_len (8*1024*1024)
typedef char (*fft_test) (Complex *, Complex *, unsigned int, char);
typedef void (*sort_test) ();

uint64_t start, stop;

void wait()
{
    uint64_t limit = 5320000000;
    MEDIR_TIEMPO_START(start);
    do {
        MEDIR_TIEMPO_STOP(stop);
    } while ((stop - start) < limit);
    return;
}

uint32_t rand()
{
    uint64_t seed = *((uint64_t *) seed_address);
    seed = seed * 1103515245 + 12345;
    *((uint64_t *) seed_address) = seed;

    return (uint32_t) (seed / 65536) % 32768;
}

void generate_global_array(uint64_t seed, uint32_t len)
{
    *((uint64_t *) seed_address) = seed;
    uint32_t *array = (uint32_t *) array_start_address;
    for (uint32_t i = 0; i < len; ++i) {
        array[i] = rand();
    }
}

bool verfiy_sort()
{
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t len = *((uint32_t *) array_len_address);
    for (uint32_t i = 1; i < len; ++i) {
        if (array[i - 1] > array[i]) {
            return false;
        }
    }
    return true;
}

void clean_array(uint32_t len)
{
    uint32_t *array = (uint32_t *) array_start_address;
    for (uint32_t i = 0; i < len; ++i) {
        array[i] = 0;
    }
}


#define TOP_RUN 23
#define TOTAL_TESTS  5

void test_suite_sort(sort_test function, uint8_t col, uint8_t line, const char *header)
{
    uint32_t *len = (uint32_t *) array_len_address;
    print_string(header, line++, col);
    int iter;
    double *run_measures = (double *) run_measures_address;
    for (int i = 0; i < TOP_RUN; ++i) {
        run_measures[i] = 0;
    }
    for (int run = 0; run < TOTAL_TESTS; ++run) {
        for (iter = 0, *len = 2; *len < max_len; *len <<= 1, iter++) {
            uint32_t seed = 13214;
            generate_global_array(seed, *len);
            MEDIR_TIEMPO_START(start);
            function();
            MEDIR_TIEMPO_STOP(stop);
            if (verfiy_sort()) {
                run_measures[iter] += stop - start;
            } else {
                print_string("bad sort :(", line++, col);
            }
        }
    }
    for (int i = 0; i < TOP_RUN; i++, line++) {
        uint64_t res = run_measures[i] / ((double) TOTAL_TESTS);
        print_number_u64(res, line, col);
    }
    print_string("Done! :D", line, col);
}

void sort_monocore()
{
    heapsort(
        (uint32_t *) array_start_address,
        *((uint32_t *) array_len_address)
    );
}

void test_1_core()
{
    clean_array(max_len);
    clear_screen();

    test_suite_sort(sort_monocore, 0, 0, "Test 1 core");

}

void test_2_cores()
{
    test_suite_sort(sort_bsp, 30, 0, "Test 2 cores");
    *((uint8_t *) sleep_address) = 1;
}

void test_ipi_cores()
{
    test_suite_sort(sort_bsp_ipi, 60, 0, "Test Dual Ipis");
}

void test_mem_sync()
{
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *sleep = (uint8_t *) sleep_address;
    uint64_t *time_measures = (uint64_t *) time_measures_address;

    uint8_t col[6] = {0, 13, 26, 39, 52, 65};
    uint8_t line = 0;
    print_string("Test 2 cores", line++, col[0]);
    print_string("Sync", line, col[1]);
    // print_string("Sync", line, col[3]);
    // print_string("Sync", line, col[5]);
    int iters = 10;
    uint64_t avgs;

    for (*len = 2; *len < (8 * 1024 * 1024); *len *= 2) {
        // uint32_t seed = 13214;
        // generate_global_array(seed, *len);
        avgs = 0;
        for (int i = 0; i < iters; ++i) {
            time_measures[0] = 0;
            measure_sync_mem();
            avgs += time_measures[0];
            /* code */
            // if (verfiy_sort()) {
            // for (uint8_t i = 1; i < 2; i += 2) {
        }
        line++;
        print_number_u64(avgs / iters, line, col[1]);
        // }
        // } else {
        //     for (uint8_t i = 1; i < 2; i += 2) {
        //         print_string("bad_sort :(", line++, col[i]);
        //     }
        // }
    }

    line++;
    for (uint8_t i = 0; i < 1; ++i) {
        print_string("Done!", line, col[i]);
    }
    *sleep = 1;
}


void test_sync_ipi_cores()
{
    wait();
    wait();
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    uint64_t *time_measures = (uint64_t *) time_measures_address;

    uint8_t col[6] = {0, 13, 26, 39, 52, 65};
    uint8_t line = 0;
    print_string("Test Dual Ipis", line++, col[0]);

    print_string("Sync", line, col[1]);
    print_string("Sync", line, col[3]);
    print_string("Sync", line, col[5]);

    for (*len = 2; *len < max_len; *len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, *len);
        measure_sync_ipis();
        if (verfiy_sort()) {
            line++;
            uint8_t i;
            for (i = 1; i < 6; i += 2) {
                print_number_u64(time_measures[i], line, col[i]);
            }
        } else {
            for (uint8_t i = 1; i < 5; i += 2) {
                print_string("bad_sort :(", line++, col[i]);
            }
        }

    }
    uint8_t i;
    line++;
    for (i = 1; i < 6; i += 2) {
        print_string("Done!", line, col[i]);
    }
}


void sum_vector(uint32_t len)
{
    uint32_t *array = (uint32_t *) array_start_address;
    for (uint32_t i = 0; i < len; ++i) {
        array[i]++;
    }
}

void test_sum_vector1()
{
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t col = 68;
    uint8_t line = 0;

    print_string("sum 1 core", line++, col);
    for (*len = 2; *len < max_len; *len *= 2) {
        MEDIR_TIEMPO_START(start);
        sum_vector(*len);
        MEDIR_TIEMPO_STOP(stop);
        print_number_u64(stop - start, line++, col);
    }
    print_string("Done! :D", ++line, col);
}

void test_sum_vector2()
{
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *sleep = (uint8_t *) sleep_address;
    uint8_t col = 102;
    uint8_t line = 0;

    print_string("sum 2 cores", line++, col);

    for (*len = 2; *len < max_len; *len *= 2) {
        MEDIR_TIEMPO_START(start);
        sum_vector_bsp();
        MEDIR_TIEMPO_STOP(stop);
        print_number_u64(stop - start, line++, col);
    }
    print_string("Done! :D", ++line, col);
    *sleep = 1;
}


////////////////////////////////////////////////////////////////////////////////////
//                              FFT                                               //
////////////////////////////////////////////////////////////////////////////////////

void generate_fft_array(uint32_t N)
{
    Complex *array = (Complex *) array_start_address;
    for (uint32_t i = 0; i < N / 2; ++i) {
        array[i].m_re = 1.0;
        array[i].m_im = 0.0;
    }
    for (uint32_t i = N / 2; i < N; ++i) {
        array[i].m_re = 0.0;
        array[i].m_im = 0.0;
    }
}

static double abs(double a)
{
    return a < 0.0 ? -a : a;
}

bool cmp_complex_arrays(Complex *a, Complex *b, uint32_t N, double error)
{
    double diff;
    for (uint32_t i = 0; i < N; ++i) {

        a[i].m_re = abs(a[i].m_re);
        b[i].m_re = abs(b[i].m_re);

        diff = abs(a[i].m_re - b[i].m_re);

        if (diff > error) {
            return false;
        }

        a[i].m_im = abs(a[i].m_im);
        b[i].m_im = abs(b[i].m_im);

        diff = abs(a[i].m_im - b[i].m_im);

        if (diff > error) {
            return false;
        }
    }
    return true;
}

bool verifiy_fft(Complex *Input, Complex *Output, uint32_t N)
{
    Complex *Output2 = (Complex *) (array_start_address + 32 * TEN_MEGA);
    Forward_IO(Output, Output2, N);
    double error = 0.01;
    return cmp_complex_arrays(Input, Output2, N, error);
}

#define MAX_FFT_LEN  (8 * 1024* 1024)
#define TOP_RUN_FFT  20

void test_suite_fft(fft_test test_to_run, uint8_t line, uint8_t col, const char *msg)
{
    uint32_t *len = (uint32_t *) array_len_address;
    breakpoint
    Complex *Input = (Complex *) array_start_address;
    Complex *Output = (Complex *) temp_address;
    bool bad_fft = false;
    print_string(msg, line++, col);

    int iter;
    double measure;

    for (iter = 0, *len = 64, measure = 0;
            *len <= MAX_FFT_LEN;
            *len <<= 1, iter++, measure = 0) {

        for (int run = 0; run < TOTAL_TESTS && !bad_fft; ++run) {
            generate_fft_array(*len);
            MEDIR_TIEMPO_START(start);
            test_to_run(Input, Output, *len, TRUE);
            MEDIR_TIEMPO_STOP(stop);
            if (verifiy_fft(Input, Output, *len)) {
                measure += stop - start;
            } else {
                print_string("bad_fft :(", line, col);
                bad_fft = true;
            }
        }

        uint64_t res = measure / ((double) TOTAL_TESTS);
        if (!bad_fft) {
            print_number_u64(res, line, col);
        }
        line++;
        bad_fft = false;
    }

    print_string("Done! :D", ++line, col);
}


void test_fft_mono()
{
    // clear_screen();
    test_suite_fft(Inverse_IO, 0, 0, "fft monocore");
}

void test_fft_dual_mem()
{
    clear_screen();
    test_suite_fft(Inverse_IO_Dual, 0, 30, "fft dualcore");
    *((uint8_t *) sleep_address) = 1;
}

void test_fft_dual_ipi()
{
    test_suite_fft(Inverse_IO_Ipi, 0, 60, "fft dualcore ipis");
}

