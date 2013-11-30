#ifndef __ASSERT_H
#define __ASSERT_H

#define fail_if(c,...)\
	if(c) kernel_panic("Paso que " #c,#__VA_ARGS__);

#define fail_unless(c,...)\
	if(!(c)) kernel_panic("No paso que " #c,#__VA_ARGS__);


#endif
