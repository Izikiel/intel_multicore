#ifndef __ASSERT_H
#define __ASSERT_H

#include <scrn.h>
#include <irq.h>

#define kernel_panic(m,...)\
	do{ irq_cli(); scrn_cls();\
		scrn_printf("KERNEL PANIC (%s:%d):\n\t " m,\
			__FILE__,__LINE__,## __VA_ARGS__); while(1); }while(0)

#define fail_if(c,...)\
	if(c) kernel_panic("Paso que " #c,#__VA_ARGS__);

#define fail_unless(c,...)\
	if(!(c)) kernel_panic("No paso que " #c,#__VA_ARGS__);


#endif
