#ifndef AUX_HH
#define AUX_HH

/* preform the heapsort */
void heapsort(int ar[], int len);
/* help heapsort() to bubble down starting at pos[ition] */
void heapbubble(int pos, int ar[], int len);
/* merge function */
void merge(int a[], int low, int high, int mid, int buffer[]);
void limit_merge(int a[], int c[], int low, int high, int mid, int size);
void limit_merge_reverse(int a[], int c[], int low, int high, int mid, int size);
/* copy vectors */
void copy(int a[], int ia, int b[], int ib, int size);
/* print */
void print(int ar[], int len);
/* aux */
void swap(int ar[], int i, int j);

#endif
