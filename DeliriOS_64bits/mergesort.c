#include "types.h"
#include "i386.h"

uint8_t array_global[] = {'z', 's', 'x', 'j', 'k', 'n', 'q', 'l',
 'b', 'c', 'h', 'g', 'u', 'p', 'i', 'r', 'w', 'a', 'y', 'm', 'e',
  'o', 'd', 'f', 't', 'v', 'V', 'K', 'B', 'U', 'S', 'P', 'M', 'N',
   'W', 'D', 'C', 'O', 'A', 'L', 'G', 'H', 'F', 'Y', 'R', 'J', 'I',
    'Z', 'T', 'Q', 'X', 'E'};

uint8_t temp1[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

uint8_t temp2[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

const uint32_t start_point = 26;
const uint32_t arr_len = 52;

uint8_t start_merge = 0;

uint8_t start = 0;
uint8_t done = 0;

uint32_t len;
uint32_t half_len;
uint32_t full_array;

uint8_t finish = 0;


void mergesort(uint32_t* array, uint32_t len){

    if (len <= 1) {
        return;
    }

    uint32_t len1 = len/2;
    uint32_t len2 = len%2 ? (len+1)/2 : len/2;
    uint32_t* half1 = array;
    uint32_t* half2 = array + len1;
    mergesort(half1, len1, id);
    mergesort(half2, len2, id);
    uint32_t result[len];

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

void swap(uint32_t ar[], uint32_t i, uint32_t j) { uint32_t t=ar[i]; ar[i]=ar[j]; ar[j]=t; };

void limit_merge(uint32_t arr[], uint32_t copy[], uint32_t low, uint32_t mid, uint32_t high, uint32_t size) {
        uint32_t i,j,k,l,s=size;
        l=low;
        i=0;
        j=mid+1;
        while((l<=mid)&&(j<=high)&&(s>0)) {
                if(arr[l]<=arr[j]) {
                                copy[i]=arr[l];
                                l++;
                        } else {
                                copy[i]=arr[j];
                                j++;
                        }
                i++;
                s--;
        }
        if(s!=0) {
                if(l>mid) {
                        for(k=j;k<=high;k++) {
                                copy[i]=arr[k];
                                i++;
                                s--;
                                if(s>0) break;
                        }
                } else {
                        for(k=l;k<=mid;k++) {
                                copy[i]=arr[k];
                                i++;
                                s--;
                                if(s>0) break;
                          }
                }
        }
}

void limit_merge_reverse(uint32_t arr[], uint32_t copy[], uint32_t low, uint32_t mid, uint32_t high, uint32_t size) {
        uint32_t i,j,k,l,s=size;
        l=mid;
        i=size-1;
        j=high;
        while((low<=l)&&(mid+1<=j)&&(s>0)) {
                if(arr[l]>=arr[j]) {
                                copy[i]=arr[l];
                                l--;
                        } else {
                                copy[i]=arr[j];
                                j--;
                        }
                i--;
                s--;
        }
        if(s!=0) {
                if(l>mid) {
                        for(k=j;k>=mid+1;k--) {
                                copy[i]=arr[k];
                                i--;
                                s--;
                                if(s>0) break;
                        }
                } else {
                        for(k=l;k>=low;k--) {
                                copy[i]=arr[k];
                                i--;
                                s--;
                                if(s>0) break;
                          }
                }
        }
}

void copy(uint32_t a[], uint32_t ia, uint32_t b[], uint32_t ib, uint32_t size) {
  uint32_t i;
  for(i=0;i<size;i++){
    a[ia] = b[ib];
    ia++;
    ib++;
  }
}


void mergesort_pm(){
    uint32_t* half1 = array_global;
    start = 1;
    mergesort(half1, start_point);
    for(;!done;);
    done = 0;
    start_merge = 1;
    limit_merge(array_global, temp1, 0, start_point-1, arr_len-1, start_point);
    for(;!done;);
    done = 0;
    start = 0;
    start_merge = 0;

    copy(array_global, 0, temp1, 0, start_point);

}

void do_reverse_merge(){
    for (;!start_merge;);
    limit_merge_reverse(array_global, temp2, 0, start_point-1, arr_len-1, start_point);
    copy(array_global, start_point, temp2, 0, start_point);
    done = 1;
}


void mergesort_core1(){
    uint32_t temp_array_1[half_len];
    uint32_t* half_array = full_array;

    start = 1;
    mergesort(half_array, half_len);
    for(;!done;);

    start = 0;
    done = 0;
    start_merge = 1;

    limit_merge(full_array, temp_array_1, 0, (half_len) - 1, len - 1, half_len);
    for(;!done;);

    start_merge = 0;

    done = 0;

    copy(full_array, 0, temp_array_1, 0, len);

}

void mergesort_core2(){
    for(;!finish;){

        for(;!start;);

        uint32_t* half_array = full_array + half_len;

        mergesort(half_array, half_len);
        done = 1;
       
        for (;!start_merge;);

        uint32_t temp_array_2[half_len];

        limit_merge_reverse(full_array, temp_array_2, 0, half_len - 1, len - 1, half_len);

        copy(full_array, half_len, temp_array_2, 0, half_len);

        done = 1;
    }

}

