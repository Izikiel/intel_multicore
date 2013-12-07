#ifndef __TIMER_H
#define __TIMER_H

#include <types.h>
#include <proc.h>
#include <utils.h>
#include <ports.h>
#include <irq.h>

extern void init_timer(uint);
extern void timer_tick(uint,gen_regs);
extern void core_sleep(uint);

#endif
