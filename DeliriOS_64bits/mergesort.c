#include "types.h"

uint8_t array_global[] = {24, 62, 46, 64, 55, 18, 84, 98, 82, 76, 60,
 28, 30, 85, 99, 0, 96, 7, 34, 31, 80, 5, 89, 78, 43, 57, 8, 97,
 66, 2, 69, 93, 70, 21, 90, 37, 67, 47, 68, 94, 29, 59, 12, 58,
 79, 86, 71, 15, 11, 32, 39, 74, 22, 36, 72, 17, 65, 54, 38, 4,
 73, 19, 88, 13, 1, 45, 51, 23, 42, 49, 41, 9, 91, 53, 48, 14, 52,
 6, 10, 40, 83, 81, 3, 20, 25, 26, 35, 16, 75, 33, 27, 50, 56, 92,
 87, 77, 44, 61, 95, 63};

uint8_t start_point = 50;

volatile uint8_t start = 0;
volatile uint8_t done = 0;

void mergesort(uint8_t* array, uint32_t len){

    if (len <= 1) {
        return;
    }

    uint32_t len1 = len/2;
    uint32_t len2 = len%2 ? (len+1)/2 : len/2;
    mergesort(array, len1);
    mergesort(array + len2, len2);
    uint8_t* half1 = array;
    uint8_t* half2 = array + len2;
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
    mergesort(half1, 50);
    start = 0;
}