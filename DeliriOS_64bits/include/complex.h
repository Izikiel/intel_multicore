
#ifndef _COMPLEX_H_
#define _COMPLEX_H_

#define FALSE 0
#define TRUE  1

    typedef struct s_complex {
        double m_re;
        double m_im;
    } Complex;


    Complex complex(double re, double im);
    double re(Complex* c);
    double im(Complex* c);
    double norm(Complex* c);
    Complex operatorADD (Complex* op1, Complex* op2);
    Complex operatorSUB (Complex* op1, Complex* op2);
    Complex operatorMUL (Complex* op1, Complex* op2);
    Complex operatorDIV (Complex* op1, Complex* op2);

#endif