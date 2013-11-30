#ifndef __PORTS_H
#define __PORTS_H

#include <types.h>

//Lee un byte de un puerto
extern uchar inb (ushort);
//Lee un short de un puerto
extern ushort inw (ushort);
//Escribe un byte a un puerto un valor
extern void outb (ushort, uchar);
//Escribe un short a un puerto un valor
extern void outw (ushort, ushort);

#endif
