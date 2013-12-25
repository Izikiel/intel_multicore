#ifndef __TIMER_H__
#define __TIMER_H__
#include <types.h>

#define MAX_INSTANCES 32

void initialize_timer();
uint64_t getFreeInstance();
bool sleep(const uint64_t ticksCount);
void timer_tick();

#endif  /* !__TIMER_H__ */
