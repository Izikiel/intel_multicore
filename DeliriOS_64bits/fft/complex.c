#include "complex.h"

    Complex complex(double re, double im){
        Complex c;
        c.m_re = re;
        c.m_im = im;
        return c;
    }

    double re(Complex* c) { return c->m_re; }
    double im(Complex* c) { return c->m_im; }

    double norm(Complex* c) {
        return c->m_re * c->m_re + c->m_im * c->m_im;
    }

    Complex operatorADD (Complex* op1, Complex* op2) {
        return complex(op1->m_re + op2->m_re, op1->m_im + op2->m_im);
    }

    Complex operatorSUB (Complex* op1, Complex* op2) {
        return complex(op1->m_re - op2->m_re, op1->m_im - op2->m_im);
    }

    Complex operatorMUL (Complex* op1, Complex* op2) {
        return complex(op1->m_re * op2->m_re - op1->m_im * op2->m_im,
                       op1->m_re * op2->m_im + op1->m_im * op2->m_re);
    }

    Complex operatorDIV (Complex* op1, Complex* op2) {
        const double denominator = op2->m_re * op2->m_re + op2->m_im * op2->m_im;
        return complex((op1->m_re * op2->m_re + op1->m_im * op2->m_im) / denominator,
                       (op1->m_im * op2->m_re - op1->m_re * op2->m_im) / denominator);
    }