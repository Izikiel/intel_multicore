#include <stdio.h>
#include <math.h>

#include "fft.h"
#include "complex.h"

#define _USE_MATH_DEFINES

// using namespace std;

int main() {
    
int i;
const unsigned int N = 128;
Complex Input[N];
Complex Output[N];
Complex Output2[N];
double reI,imI,reO,imO,mg,fa,reO2,imO2;
char a,b;
char scale;

// for(int i=0;i<N;i++) {
//     double ii = sin(((double)i)*M_PI/32);
//     Input[i] = ii;
// }

for(i=0;i<N/2;i++) {
    Input[i] = complex(1.0,0.0);
}
for(i=N/2;i<N;i++) {
    Input[i] = complex(0.0,0.0);
}

a = Inverse_IO(Input,Output,N,TRUE);

// for(i=N/2;i<N;i++) {
//     reO = Output[i].m_re;
//     imO = Output[i].m_im;
//     mg = sqrt(reO*reO+imO*imO);
//     if(mg<0.1) Output[i] = complex(0.0,0.0);
// }

b = Forward_IO(Output,Output2,N);

for(i=0;i<N;i++) {
    reI = Input[i].m_re;
    imI = Input[i].m_im;
    reO = Output[i].m_re;
    imO = Output[i].m_im;
    reO2 = Output2[i].m_re;
    imO2 = Output2[i].m_im;
    mg = sqrt(reO*reO+imO*imO);
    fa = atan(imO/reO);
    
    printf("%f\t%f\t%f\t%f\t%f\t%f\t%f\n",reI,reO,imO,reO2,imO2,mg,fa);
}

    return 0;

}
