#ifndef _FFT_H_
#define _FFT_H_

 #include "complex.h"
 #include "defines.h"
 #include "types.h"
// ================== FFT ==================

    //   FORWARD FOURIER TRANSFORM
    //     Input  - input data
    //     Output - transform result
    //     N      - length of both input data and result
        char Forward_IO(Complex *Input, Complex *Output, unsigned int N);

    //   FORWARD FOURIER TRANSFORM, INPLACE VERSION
    //     Data - both input data and output
    //     N    - length of input data
        char Forward(Complex *Data, unsigned int N);

    //   INVERSE FOURIER TRANSFORM
    //     Input  - input data
    //     Output - transform result
    //     N      - length of both input data and result
    //     Scale  - if to scale result
        char Inverse_IO(Complex *Input, Complex *Output, unsigned int N, char Scale ); /*true*/

    //  Dual core version
        char Inverse_IO_Dual(Complex *Input, Complex *Output, unsigned int N, char Scale ); /*true*/

    //   INVERSE FOURIER TRANSFORM, INPLACE VERSION
    //     Data  - both input data and output
    //     N     - length of both input data and result
    //     Scale - if to scale result
        char Inverse(Complex *Data, unsigned int N, char Scale ); /*true*/

    // ------------------ AUX ------------------

    //   Rearrange function and its inplace version
        void Rearrange_IO(Complex *Input, Complex *Output, unsigned int N);
        void Rearrange(Complex *Data, unsigned int N);

    //   FFT implementation
        void Perform(Complex *Data, unsigned int N, char Inverse ); /*false*/

        void Perform_P_Mem(Complex *Data, unsigned int N, char Inverse ); /*false*/

    //   Scaling of inverse FFT result
        void Scale(Complex *Data, unsigned int N);

// =========================================

#endif
