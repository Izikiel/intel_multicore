#include "defines.h"
#include "types.h"
#include "sort_code.h"
#include "screen_utils.h"
#include "multicore_common.h"
#include "tiempo.h"
#include "fft.h"

void sort_bsp();
void sum_vector_bsp();
void sort_bsp_ipi();
void make_ap_jump();

void measure_sync_ipis();
void measure_sync_mem();
