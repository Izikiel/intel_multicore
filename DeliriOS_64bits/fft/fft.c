#include "fft.h"
#include "complex.h"

#define breakpoint __asm __volatile("xchg %%bx, %%bx" : :);

extern void check_rax();
extern double sin(double);

//   FORWARD FOURIER TRANSFORM
//     Input  - input data
//     Output - transform result
//     N      - length of both input data and result
char Forward_IO(Complex *Input, Complex *Output, unsigned int N)
{
    //   Check input parameters
    if (!Input || !Output || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Initialize data
    Rearrange_IO(Input, Output, N);
    //   Call FFT implementation
    Perform(Output, N, FALSE);
    //   Succeeded
    return TRUE;
}

//   FORWARD FOURIER TRANSFORM, INPLACE VERSION
//     Data - both input data and output
//     N    - length of input data
char Forward(Complex *Data, unsigned int N)
{
    //   Check input parameters
    if (!Data || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Rearrange
    Rearrange(Data, N);
    //   Call FFT implementation
    Perform(Data, N, FALSE);
    //   Succeeded
    return TRUE;
}

//   INVERSE FOURIER TRANSFORM
//     Input  - input data
//     Output - transform result
//     N      - length of both input data and result
//     Scale  - if to scale result
char Inverse_IO(Complex *Input, Complex *Output, unsigned int N,
                char ifScale /* = true */)
{
    //   Check input parameters
    if (!Input || !Output || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Initialize data
    Rearrange_IO(Input, Output, N);
    //   Call FFT implementation
    Perform(Output, N, TRUE);
    //   Scale if necessary
    if (ifScale) {
        Scale(Output, N);
    }
    //   Succeeded
    return TRUE;
}

//   INVERSE FOURIER TRANSFORM Dual Core
//     Input  - input data
//     Output - transform result
//     N      - length of both input data and result
//     Scale  - if to scale result
char Inverse_IO_Dual(Complex *Input, Complex *Output, unsigned int N,
                     char ifScale /* = true */)
{
    //   Check input parameters
    if (!Input || !Output || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Initialize data
    Rearrange_IO(Input, Output, N);
    //   Call FFT implementation
    Perform_P_Mem(Output, N, TRUE);
    //   Scale if necessary
    if (ifScale) {
        Scale(Output, N);
    }
    //   Succeeded
    return TRUE;
}

//   INVERSE FOURIER TRANSFORM, INPLACE VERSION
//     Data  - both input data and output
//     N     - length of both input data and result
//     Scale - if to scale result
char Inverse(Complex *Data, unsigned int N, char ifScale /* = true */)
{
    //   Check input parameters
    if (!Data || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Rearrange
    breakpoint
    Rearrange(Data, N);
    //   Call FFT implementation
    breakpoint
    Perform(Data, N, TRUE);
    //   Scale if necessary
    if (ifScale) {
        Scale(Data, N);
    }
    //   Succeeded
    return TRUE;
}

//   Rearrange function
void Rearrange_IO(Complex *Input, Complex *Output, unsigned int N)
{
    //   Data entry position
    unsigned int Position;
    unsigned int Target = 0;
    //   Process all positions of input signal
    for (Position = 0; Position < N; ++Position) {
        //  Set data entry
        Output[Target] = Input[Position];
        //   Bit mask
        unsigned int Mask = N;
        //   While bit is set
        while (Target & (Mask >>= 1))
            //   Drop bit
        {
            Target &= ~Mask;
        }
        //   The current bit is 0 - set it
        Target |= Mask;
    }
}

//   Inplace version of rearrange function
void Rearrange(Complex *Data, unsigned int N)
{
    //   Swap position
    unsigned int Position;
    unsigned int Mask;
    Complex Temp;
    unsigned int Target = 0;
    //   Process all positions of input signal
    for (Position = 0; Position < N; ++Position) {
        //   Only for not yet swapped entries
        if (Target > Position) {
            //   Swap entries
            Temp = Data[Target];
            Data[Target] = Data[Position];
            Data[Position] = Temp;
        }
        //   Bit mask
        Mask = N;
        //   While bit is set
        while (Target & (Mask >>= 1))
            //   Drop bit
        {
            Target &= ~Mask;
        }
        //   The current bit is 0 - set it
        Target |= Mask;
    }
}

//   FFT implementation
void Perform(Complex *Data, unsigned int N, char Inverse /* = false */)
{
    const double pi = Inverse ? 3.14159265358979323846 : -3.14159265358979323846;
    unsigned int Step, Group, Jump, Pair, Match;
    double delta, Sine;
    Complex Multiplier, Product, tempMul, Factor;
    //   Iteration through dyads, quadruples, octads and so on...
    for (Step = 1; Step < N; Step <<= 1) {
        //   Jump to the next entry of the same transform factor
        Jump = Step << 1;
        //   Angle increment
        delta = pi / ((double)Step);
        //   Auxiliary sin(delta / 2)
        Sine = sin(delta * .5);
        //   Multiplier for trigonometric recurrence
        Multiplier = complex(-2. * Sine * Sine, sin(delta));
        //   Start value for transform factor, fi = 0
        Factor = complex(1.0, 0.0);
        //   Iteration through groups of different transform factor
        for (Group = 0; Group < Step; ++Group) {
            //   Iteration within group
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
            //   Successive transform factor via trigonometric recurrence
            tempMul = operatorMUL(&Multiplier, &Factor);
            Factor = operatorADD(&tempMul, &Factor);
        }
    }
}


/*
    group_address
    step_address
    jump_address
    factor_address
*/
void Perform_P_Mem(Complex *Data, unsigned int N, char Inverse /* = false */)
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

    volatile uint8_t *start = (volatile uint8_t *) start_address;
    volatile uint8_t *done = (volatile uint8_t *) done_address;

    *start = 0;
    *done = 0;


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

                *done = 0;
                *start = 1;

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

                active_wait(*done);
            }

            //   Successive transform factor via trigonometric recurrence
            tempMul = operatorMUL(&Multiplier, &Factor);
            Factor = operatorADD(&tempMul, &Factor);
        }
    }
}


//   Scaling of inverse FFT result
void Scale(Complex *Data, unsigned int N)
{
    const double Factor = 1. / ((double)N);
    unsigned int Position;
    //   Scale all data entries
    for (Position = 0; Position < N; ++Position) {
        Data[Position].m_re = Data[Position].m_re * Factor;
        Data[Position].m_im = Data[Position].m_im * Factor;
    }
}

#define fft_int 43

char Inverse_IO_Ipi(Complex *Input, Complex *Output, unsigned int N,
                    char ifScale /* = true */)
{
    //   Check input parameters
    if (!Input || !Output || N < 1 || N & (N - 1)) {
        return FALSE;
    }
    //   Initialize data
    Rearrange_IO(Input, Output, N);
    //   Call FFT implementation
    Perform_P_Int(Output, N, TRUE);
    //   Scale if necessary
    if (ifScale) {
        Scale(Output, N);
    }
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
    volatile uint8_t *done = (volatile uint8_t *) done_address;
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
                *done = 1;
                check_rax();

            }
            //   Successive transform factor via trigonometric recurrence
            tempMul = operatorMUL(&Multiplier, &Factor);
            Factor = operatorADD(&tempMul, &Factor);
        }
    }
}

void inner_fft_loop()
{

    unsigned int Step;
    unsigned int Jump;
    unsigned int Group;

    volatile uint8_t *start = (volatile uint8_t *) start_address;
    volatile uint8_t *done = (volatile uint8_t *) done_address;
    volatile uint8_t *sleep = (volatile uint8_t *) sleep_address;
    Complex *Data = (Complex *) temp_address;

    *start = 0;
    *sleep = 0;
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