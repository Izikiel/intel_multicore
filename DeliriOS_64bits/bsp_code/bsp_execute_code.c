#include "bsp_execute_code.h"

extern void check_rax();
extern double sin(double);

void send_ipi_ap(uint32_t interrupt);

#define sort_ap_ipi 40
#define merge_ap_ipi 41
#define copy_ap_ipi 42
#define fft_int 43


uint64_t init, stop;

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
    uint64_t *time_measures = (uint64_t *) time_measures_address;

    //ready, set, go!
    clean_flags();

    *start = 1;
    MEDIR_TIEMPO_START(init);
    heapsort(array, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[0] = stop - init;

    MEDIR_TIEMPO_START(init);
    active_wait(*done);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[1] = stop - init;

    *done = 0;

    *start_merge = 1;
    MEDIR_TIEMPO_START(init);
    limit_merge(array, bsp_temp, 0, (*len / 2) - 1, *len - 1, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[2] = stop - init;

    MEDIR_TIEMPO_START(init);
    active_wait(*done);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[3] = stop - init;

    *done = 0;

    *start_copy = 1;
    MEDIR_TIEMPO_START(init);
    copy(array, 0, bsp_temp, 0, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[4] = stop - init;

    MEDIR_TIEMPO_START(init);
    active_wait(*finish_copy);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[5] = stop - init;

    clean_flags();

}

void sum_vector_bsp()
{
    uint8_t *start = (uint8_t *) start_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *finish = (uint8_t *) finish_copy_address;

    clean_flags();
    *start = 1;

    for (uint32_t i = 0; i < *len / 2; ++i) {
        array[i]++;
    }

    active_wait(*finish);
    clean_flags();
}

void sort_bsp_ipi()
{

    //si empieza a reventar con GP por el AP,
    //hay que cambiar el origen de linkeo pq este modulo
    //cambio el lugar de origen por su tamaño

    //synchronization flags

    uint32_t *len = (uint32_t *) array_len_address;
    uint32_t *array = (uint32_t *) array_start_address;

    uint32_t *bsp_temp = (uint32_t *) temp_address;

    uint64_t *time_measures = (uint64_t *) time_measures_address;


    //ready, set, go!
    // breakpoint
    send_ipi_ap(sort_ap_ipi);

    MEDIR_TIEMPO_START(init);
    heapsort(array, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[0] = stop - init;

    MEDIR_TIEMPO_START(init);
    check_rax();
    MEDIR_TIEMPO_STOP(stop);
    time_measures[1] = stop - init;


    // breakpoint
    send_ipi_ap(merge_ap_ipi);

    MEDIR_TIEMPO_START(init);
    limit_merge(array, bsp_temp, 0, (*len / 2) - 1, *len - 1, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[2] = stop - init;

    MEDIR_TIEMPO_START(init);
    check_rax();
    MEDIR_TIEMPO_STOP(stop);
    time_measures[3] = stop - init;

    // breakpoint
    send_ipi_ap(copy_ap_ipi);

    MEDIR_TIEMPO_START(init);
    copy(array, 0, bsp_temp, 0, *len / 2);
    MEDIR_TIEMPO_STOP(stop);
    time_measures[4] = stop - init;

    MEDIR_TIEMPO_START(init);
    check_rax();
    MEDIR_TIEMPO_STOP(stop);
    time_measures[5] = stop - init;

}

char Inverse_IO_Ipi(Complex *Input, Complex *Output, unsigned int N,
                     char ifScale /* = true */)
{
    //   Check input parameters
    if (!Input || !Output || N < 1 || N & (N - 1))
        return FALSE;
    //   Initialize data
    Rearrange_IO(Input, Output, N);
    //   Call FFT implementation
    Perform_P_Int(Output, N, TRUE);
    //   Scale if necessary
    if (ifScale)
        Scale(Output, N);
    //   Succeeded
    return TRUE;
}

void Perform_P_Int(Complex *Data, unsigned int N, char Inverse /* = false */)
{
    const double pi = Inverse ? 3.14159265358979323846 : -3.14159265358979323846;
    unsigned int Pair, Match;
    double delta, Sine;
    Complex Multiplier, Product, tempMul;

    unsigned int *Group_G = (unsigned int *) group_address;
    unsigned int *Step_G = (unsigned int *) step_address;
    unsigned int *Jump_G = (unsigned int *) jump_address;
    Complex *Factor_G = (Complex *) factor_address;

    unsigned int Group, Step, Jump;
    Complex Factor;

    //   Iteration through dyads, quadruples, octads and so on...
    for (Step = 1; Step < N; Step <<= 1) {
        //   Jump to the next entry of the same transform factor
        Jump = Step << 1;
        //   Angle increment
        delta = pi / ((double) Step);
        //   Auxiliary sin(delta / 2)
        Sine = sin(delta * .5);
        //   Multiplier for trigonometric recurrence
        Multiplier = complex(-2. * Sine * Sine, sin(delta));
        //   Start value for transform factor, fi = 0
        Factor = complex(1.0, 0.0);
        //   Iteration through groups of different transform factor
        for (Group = 0; Group < Step; ++(Group)) {

            if (Step >= N / LIMIT) {
                for (Pair = Group; Pair < N; Pair += Jump) {
                    //   Match position
                    Match = Pair + Step;
                    //   Second term of two-point transform
                    Product = operatorMUL(&Factor, &(Data[Match]));
                    //   Transform for fi + pi
                    Data[Match] = operatorSUB(&(Data[Pair]), &Product);
                    //   Transform for fi
                    Data[Pair] = operatorADD(&Product, &(Data[Pair]));
                }
            } else {
                //Iteration within group
                *Jump_G = Jump;
                *Group_G = Group;
                *Step_G = Step;
                *Factor_G = Factor;

                send_ipi_ap(fft_int);

                for (Pair = Group; Pair < N / 2; Pair += Jump) {
                    //   Match position
                    Match = Pair + Step;
                    //   Second term of two-point transform
                    Product = operatorMUL(&Factor, &(Data[Match]));
                    //   Transform for fi + pi
                    Data[Match] = operatorSUB(&(Data[Pair]), &Product);
                    //   Transform for fi
                    Data[Pair] = operatorADD(&Product, &(Data[Pair]));

                }
                check_rax();

            }
            //   Successive transform factor via trigonometric recurrence
            tempMul = operatorMUL(&Multiplier, &Factor);
            Factor = operatorADD(&tempMul, &Factor);
        }
    }
}


void make_ap_jump()
{
    intr_command_register icr;
    initialize_ipi_options(&icr, FIXED, 39, 1);
    icr.destination_shorthand = 3;
    send_ipi(&icr);
    wait_for_ipi_reception();
    print_string("Jumpeo?", 1, 0);
}

void send_ipi_ap(uint32_t interrupt)
{
    intr_command_register icr;
    initialize_ipi_options(&icr, FIXED, interrupt, 1);
    icr.destination_shorthand = 3;
    send_ipi(&icr);
    wait_for_ipi_reception();
}

