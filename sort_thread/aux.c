#include "aux.h"

#include <stdio.h>
#include <stdlib.h>

void swap(int ar[], int i, int j) { int t=ar[i]; ar[i]=ar[j]; ar[j]=t; };

void merge(int arr[], int low, int mid, int high, int b[]) {
        int i,j,k,l; //,b[bufferN];
        l=low;
        i=low;
        j=mid+1;
        while((l<=mid)&&(j<=high)) {
                if(arr[l]<=arr[j]) {
                                b[i]=arr[l];
                                l++;
                        } else {
                                b[i]=arr[j];
                                j++;
                        }
                i++;
        }
        if(l>mid) {
                for(k=j;k<=high;k++) {
                        b[i]=arr[k];
                        i++;
                }
        } else {
                for(k=l;k<=mid;k++) {
                        b[i]=arr[k];
                        i++;
                  }
        }
        for(k=low;k<=high;k++) {
                arr[k]=b[k];
        }
} 


void limit_merge(int arr[], int copy[], int low, int mid, int high, int size) {
        int i,j,k,l,s=size;
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

void limit_merge_reverse(int arr[], int copy[], int low, int mid, int high, int size) {
        int i,j,k,l,s=size;
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

void copy(int a[], int ia, int b[], int ib, int size) {
  int i;
  for(i=0;i<size;i++){
    a[ia] = b[ib];
    ia++;
    ib++;
  }
}

void heapbubble(int pos, int array[], int len) {
 int z = 0;
 int max = 0; 
 int tmp = 0;
 int left = 0; 
 int right = 0; 

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

void heapsort(int array[], int len) {
 int i = 0;
 int tmp = 0;

 for(i = len / 2; i >= 0; --i)
  heapbubble(i, array, len); 

 for(i = len - 1; i > 0; i--) {
  tmp = array[0];  
  array[0] = array[i]; 
  array[i] = tmp;
  heapbubble(0, array, i);
 }
}

void print(int ar[], int len) { 
        int i;
      for(i=0;i<len-1;i++) {
              printf("%i ",ar[i]);
      }
      printf("%i\n",ar[len-1]);
        
//        for(i=0;i<24;i++)  printf("%i ",ar[i]); printf("%i\n",ar[24]);
//        for(i=25;i<49;i++) printf("%i ",ar[i]); printf("%i\n",ar[49]);
//        for(i=50;i<74;i++) printf("%i ",ar[i]); printf("%i\n",ar[74]);
//        for(i=75;i<99;i++) printf("%i ",ar[i]); printf("%i\n\n",ar[99]);
}
