#ifndef __ASSERT_H
#define __ASSERT_H

#include <kmain64.h>

#define fail_if(c)\
	if(c) kernel_panic("Paso que " #c);

#define fail_unless(c)\
	if(!(c)) kernel_panic("No paso que " #c);

#endif