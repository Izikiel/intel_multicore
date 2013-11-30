#include <ports.h>
#include <types.h>

uchar inb(ushort _port)
{
	uchar rv;
	__asm__ __volatile__("inb %1, %0" : "=a"(rv) : "dN"(_port));
	return rv;
}

ushort inw(ushort _port)
{
	ushort rv;
	__asm__ __volatile__("inw %1, %0" : "=a"(rv) : "dN"(_port));
	return rv;
}

void outb(ushort _port, uchar _data)
{
	__asm__ __volatile__("outb %1, %0" : : "dN"(_port), "a"(_data));
}

void outw(ushort _port, ushort _data)
{
	__asm__ __volatile__("outw %1, %0" : : "dN"(_port), "a"(_data));
}
