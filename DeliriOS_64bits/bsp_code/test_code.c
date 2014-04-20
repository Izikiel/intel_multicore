#include "types.h"
#include "tiempo.h"
#include "defines.h"
#include "sort_code.h"
#include "bsp_execute_code.h"
#include "screen_utils.h"

#include "fft.h"
#include "complex.h"

#define max_len (8*1024*1024)

uint64_t start, stop;

void wait()
{
    uint64_t limit = 532000000;
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
    //breakpoint
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
    for (uint32_t i = 0; i < len; ++i)
        array[i] = 0;
}


void generate_fft_array(uint32_t N)
{
    Complex *array = (Complex *) array_start_address;
    for (uint32_t i = 0; i < N / 2; ++i) {
        array[i].m_re = 1.0;
        array[i].m_im = 0.0;
    }
    for (uint32_t i = N/2; i < N; ++i)
    {
        array[i].m_re = 0.0;
        array[i].m_im = 0.0;
    }
}


void test_1_core()
{
    // wait();
    clean_array(max_len);
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint8_t line = 0;
    uint8_t col = 0;

    print_string("sort 1 core", line++, col);
    for (*len = 2; *len < max_len; *len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, *len);
        MEDIR_TIEMPO_START(start);
        heapsort(array, *len);
        MEDIR_TIEMPO_STOP(stop);
        if (verfiy_sort()) {
            print_number_u64(stop - start, line++, col);
        } else {
            print_string("bad_sort :(", line++, col);
        }
    }
    print_string("Done! :D", line, col);

    //breakpoint
}

void test_2_cores()
{
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *sleep = (uint8_t *) sleep_address;
    uint64_t *time_measures = (uint64_t *) time_measures_address;

    uint8_t col[6] = {0, 13, 26, 39, 52, 65};
    // uint8_t col  = 30;
    uint8_t line = 0;
    print_string("Test 2 cores", line++, col[0]);
    print_string("Sort", line, col[0]);
    print_string("Sync", line, col[1]);
    print_string("Merge", line, col[2]);
    print_string("Sync", line, col[3]);
    print_string("Copy", line, col[4]);
    print_string("Sync", line, col[5]);

    for (*len = 2; *len < max_len; *len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, *len);
        MEDIR_TIEMPO_START(start);
        sort_bsp();
        MEDIR_TIEMPO_STOP(stop);
        if (verfiy_sort()) {
            line++;
            for (uint8_t i = 0; i < 6; ++i) {
                print_number_u64(time_measures[i], line, col[i]);
            }
            // print_number_u64(stop - start, line++, col);
        } else {
            for (uint8_t i = 0; i < 6; ++i) {
                print_string("bad_sort :(", line++, col[i]);
            }
        }

    }
    line++;
    for (uint8_t i = 0; i < 6; ++i) {
        print_string("Done!", line, col[i]);
    }
    *sleep = 1;
}

void test_ipi_cores()
{
    wait();
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    // uint8_t *sleep = (uint8_t *) sleep_address;
    uint64_t *time_measures = (uint64_t *) time_measures_address;

    uint8_t col[6] = {0, 13, 26, 39, 52, 65};
    //uint8_t col = 60;
    uint8_t line = 0;
    print_string("Test Dual Ipis", line++, col[0]);

    print_string("Sort", line, col[0]);
    print_string("Sync", line, col[1]);
    print_string("Merge", line, col[2]);
    print_string("Sync", line, col[3]);
    print_string("Copy", line, col[4]);
    print_string("Sync", line, col[5]);

    for (*len = 2; *len < max_len; *len *= 2) {
        uint32_t seed = 13214;
        generate_global_array(seed, *len);
        MEDIR_TIEMPO_START(start);
        sort_bsp_ipi();
        MEDIR_TIEMPO_STOP(stop);
        if (verfiy_sort()) {
            line++;
            uint8_t i;
            for (i = 0; i < 6; ++i) {
                print_number_u64(time_measures[i], line, col[i]);
            }
            // print_number_u64(stop - start, line++, col);
        } else {
            // print_string("bad_sort :(", line++, col);
            for (uint8_t i = 0; i < 6; ++i) {
                print_string("bad_sort :(", line++, col[i]);
            }
        }

    }
    uint8_t i;
    line++;
    for (i = 0; i < 6; ++i) {
        print_string("Done!", line, col[i]);
    }
    // *sleep = 1;
}

static double abs(double a){
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
    Complex *Output2 = (Complex *) (temp_address + TEN_MEGA);
    Forward_IO(Output, Output2, N);
    double error = 0.1;
    return cmp_complex_arrays(Input, Output2, N, error);
}

#define MAX_FFT_LEN  (32*1024)
void test_fft_mono()
{
    wait();
    clear_screen();
    uint32_t *len = (uint32_t *) array_len_address;
    Complex *Input = (Complex *) array_start_address;
    Complex *Output = (Complex *) temp_address;

    uint8_t line = 0;
    uint8_t col = 0;

    print_string("fft monocore", line++, col);
    for (*len = 2; *len < MAX_FFT_LEN; *len *= 2) {
        generate_fft_array(*len);
        MEDIR_TIEMPO_START(start);
        Inverse_IO(Input, Output, *len, TRUE);
        MEDIR_TIEMPO_STOP(stop);
        if (verifiy_fft(Input, Output, *len)) {
            print_number_u64(stop - start, line++, col);
        } else {
            print_string("bad_fft :(", line++, col);
        }
    }
    print_string("Done! :D", ++line, col);
}

void test_fft_dual_mem()
{
    uint32_t *len = (uint32_t *) array_len_address;
    Complex *Input = (Complex *) array_start_address;
    Complex *Output = (Complex *) temp_address;
    uint8_t *sleep = (uint8_t *) sleep_address;

    uint8_t line = 0;
    uint8_t col = 30;

    print_string("fft dualcore", line++, col);
    for (*len = 2; *len < MAX_FFT_LEN; *len *= 2) {
        generate_fft_array(*len);
        MEDIR_TIEMPO_START(start);
        Inverse_IO_Dual(Input, Output, *len, TRUE);
        MEDIR_TIEMPO_STOP(stop);
        if (verifiy_fft(Input, Output, *len)) {
            print_number_u64(stop - start, line++, col);
        } else {
            print_string("bad_fft :(", line++, col);
        }
    }
    print_string("Done! :D", ++line, col);
    *sleep = 1;
}

void test_fft_dual_ipi()
{
    uint32_t *len = (uint32_t *) array_len_address;
    Complex *Input = (Complex *) array_start_address;
    Complex *Output = (Complex *) temp_address;

    uint8_t line = 0;
    uint8_t col = 60;

    print_string("fft dualcore ipis", line++, col);
    for (*len = 2; *len < MAX_FFT_LEN; *len *= 2) {
        generate_fft_array(*len);
        MEDIR_TIEMPO_START(start);
        Inverse_IO_Ipi(Input, Output, *len, TRUE);
        MEDIR_TIEMPO_STOP(stop);
        if (verifiy_fft(Input, Output, *len)) {
            print_number_u64(stop - start, line++, col);
        } else {
            print_string("bad_fft :(", line++, col);
        }
    }
    print_string("Done! :D", ++line, col);
}

void sum_vector(uint32_t len)
{
    uint32_t *array = (uint32_t *) array_start_address;
    for (uint32_t i = 0; i < len; ++i)
        array[i]++;
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
