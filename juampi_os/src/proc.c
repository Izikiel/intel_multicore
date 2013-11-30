#include <proc.h>
#include <utils.h>

short get_tr()
{
	ushort res;
	__asm__ __volatile__("str %0" : "=r"(res));
	return res;
}

void set_tr(short tr)
{
	__asm__ __volatile__("ltr %0" :: "r"(tr));
}

short get_cs()
{
	ushort res;
	__asm__ __volatile__("mov %%cs, %0": "=r"(res));
	return res;
}

uint get_eflags()
{
	uint res;
	__asm__ __volatile__("	pushf\n\t"
	                     "	pop %%eax\n\t"
	                     "	mov %%eax,%0"
	                     : "=r"(res)
	                     :: "eax");
	return res;
}
