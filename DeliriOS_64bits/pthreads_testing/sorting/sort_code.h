#include "types.h"
//#include "defines.h"

/* preform the heapsort */
void heapsort(uint32_t ar[], uint32_t len);
/* help heapsort() to bubble down starting at pos[ition] */
void heapbubble(uint32_t pos, uint32_t ar[], uint32_t len);
/* merge function */
void merge(uint32_t a[], uint32_t low, uint32_t high, uint32_t mid, uint32_t buffer[]);
void limit_merge(uint32_t a[], uint32_t c[], uint32_t low, uint32_t high, uint32_t mid, uint32_t size);
void limit_merge_reverse(uint32_t a[], uint32_t c[], uint32_t low, uint32_t high, uint32_t mid, uint32_t size);
/* copy vectors */
void copy(uint32_t a[], uint32_t ia, uint32_t b[], uint32_t ib, uint32_t size);
/* aux */
void swap(uint32_t ar[], uint32_t i, uint32_t j);

