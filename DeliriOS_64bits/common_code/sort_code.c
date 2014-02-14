#include "sort_code.h"

void swap(uint32_t ar[], uint32_t i, uint32_t j) { uint32_t t=ar[i]; ar[i]=ar[j]; ar[j]=t; };

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

void copy(uint32_t a[], uint32_t ia, uint32_t b[], uint32_t ib, uint32_t size) {
  uint32_t i;
  for(i=0;i<size;i++){
    a[ia] = b[ib];
    ia++;
    ib++;
  }
}

void heapbubble(uint32_t pos, uint32_t array[], uint32_t len) {
 uint32_t z = 0;
 uint32_t max = 0;
 uint32_t tmp = 0;
 uint32_t left = 0;
 uint32_t right = 0;

 z = pos;
 for(;;) {
  left = 2 * z + 1;
  right = left + 1;

  if(left >= len)
   return;
  else if(right >= len)
   max = left;
  else if(array[left] > array[right])
   max = left;
  else
   max = right;

  if(array[z] > array[max])
   return;

  tmp = array[z];
  array[z] = array[max];
  array[max] = tmp;
  z = max;
 }
}

void heapsort(uint32_t array[], uint32_t len) {
 int64_t i = 0;
 uint32_t tmp = 0;
 for(i = len / 2; i >= 0; --i){
  heapbubble(i, array, len);
 }

 for(i = len - 1; i > 0; i--) {
  tmp = array[0];
  array[0] = array[i];
  array[i] = tmp;
  heapbubble(0, array, i);
 }

}