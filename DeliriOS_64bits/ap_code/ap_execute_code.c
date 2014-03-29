#include "ap_execute_code.h"
void signal_finished();

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
    *sleep = 0;
    active_wait(*sleep) {
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
    *sleep = 0;
}

void sum_vector_ap()
{
    uint8_t *start = (uint8_t *) start_address;
    uint32_t *array = (uint32_t *) array_start_address;
    uint32_t *len = (uint32_t *) array_len_address;
    uint8_t *sleep = (uint8_t *) sleep_address;


    active_wait(*start) {
        if (*sleep) {
            return;
        }
    }

    for (uint32_t i = *len / 2; i < *len; ++i) {
        array[i]++;
    }

    *((uint8_t *) finish_copy_address) = 1;
}


void signal_finished()
{
    uint8_t *done = (uint8_t *) done_address;
    while (!(*done));
    *done = 0;

    intr_command_register icr;
    initialize_ipi_options(&icr, FIXED, 34, 0);
    send_ipi(&icr);
    wait_for_ipi_reception();
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
    clear_screen();
    print_string("Soy un ap que salta", 0, 0);
}


void inner_fft_loop()
{

    unsigned int Step;
    unsigned int Jump;
    unsigned int Group;

    uint8_t *start = (uint8_t *) start_address;
    uint8_t *done = (uint8_t *) done_address;
    uint8_t *sleep = (uint8_t *) sleep_address;
    Complex *Data = (Complex *) temp_address;

    *start = 0;
    active_wait(*sleep) {
        active_wait(*start) {
            if (*sleep) {
                return;
            }
        }
        *start = 0;
        unsigned int Match;
        unsigned int Pair;

        Step = *((unsigned int *) step_address);
        Jump = *((unsigned int *) jump_address);
        Group = *((unsigned int *) group_address);
        Complex Factor = *((Complex *) factor_address);
        uint32_t N = *((uint32_t *) array_len_address);

        for (Pair = (Group + N / 2); Pair < N; Pair += Jump) {
            //   Match position
            Match = Pair + Step;
            //   Second term of two-point transform
            Complex Product = operatorMUL(&Factor, &(Data[Match]));
            //   Transform for fi + pi
            Data[Match] = operatorSUB(&(Data[Pair]), &Product);
            //   Transform for fi
            Data[Pair] = operatorADD(&Product, &(Data[Pair]));
        }
        *done = 1;
    }
    *sleep = 0;
}


void inner_fft_loop_int()
{
    Complex *Data = (Complex *) temp_address;

    unsigned int Match;
    unsigned int Pair;

    unsigned int Step = *((unsigned int *) step_address);
    unsigned int Jump = *((unsigned int *) jump_address);
    unsigned int Group = *((unsigned int *) group_address);
    Complex Factor = *((Complex *) factor_address);
    uint32_t N = *((uint32_t *) array_len_address);

    for (Pair = Group + N / 2; Pair < N; Pair += Jump) {
        //   Match position
        Match = Pair + Step;
        //   Second term of two-point transform
        Complex Product = operatorMUL(&Factor, &(Data[Match]));
        //   Transform for fi + pi
        Data[Match] = operatorSUB(&(Data[Pair]), &Product);
        //   Transform for fi
        Data[Pair] = operatorADD(&Product, &(Data[Pair]));

    }
    signal_finished();
}