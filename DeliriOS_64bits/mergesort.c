#include "types.h"

uint8_t array_global[] = {'z', 's', 'x', 'j', 'k', 'n', 'q', 'l',
 'b', 'c', 'h', 'g', 'u', 'p', 'i', 'r', 'w', 'a', 'y', 'm', 'e',
  'o', 'd', 'f', 't', 'v', 'V', 'K', 'B', 'U', 'S', 'P', 'M', 'N',
   'W', 'D', 'C', 'O', 'A', 'L', 'G', 'H', 'F', 'Y', 'R', 'J', 'I',
    'Z', 'T', 'Q', 'X', 'E'};

uint8_t start_point = 26;

volatile uint8_t start = 0;
volatile uint8_t done = 0;

void mergesort(uint8_t* array, uint32_t len){

    if (len <= 1) {
        return;
    }

    uint32_t len1 = len/2;
    uint32_t len2 = len%2 ? (len+1)/2 : len/2;
    uint8_t* half1 = array;
    uint8_t* half2 = array + len1;
    mergesort(half1, len1);
    mergesort(half2, len2);
    uint8_t result[len];

    uint32_t i= 0; uint32_t j= 0; uint32_t k = 0;

    for (i = 0; i < len; i++){
		if(j < len1 && k < len2) {
			if (half1[j] < half2[k]) {
				result[i] = half1[j];
				j++;
			}
			else{
				result[i] = half2[k];
				k++;
			}
		}
		else{
			break;
		}
    }
    if(j == len1 && k < len2){
    	for(;k < len2; k++, i++)
    		result[i] = half2[k];
    }
    if(k == len2 && j < len1){
		for(;j < len1; j++, i++)
			result[i] = half1[j];
	}
    for(i = 0; i < len; i++)
    	array[i] = result[i];

    return;
}

void mergesort_pm(){
    uint8_t* half1 = array_global;
    start = 1;
    mergesort(half1, start_point);
    for(;!done;);
    //start = 0;
}