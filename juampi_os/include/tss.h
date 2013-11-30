#ifndef __TSS_H
#define __TSS_H

#include "types.h"

typedef struct {
	ushort prev_task;
	ushort __reserved1;
	uint esp0;
	ushort ss0;
	ushort __reserved2;
	uint esp1;
	ushort ss1;
	ushort __reserved3;
	uint esp2;
	ushort ss2;
	ushort __reserved4;
	uint cr3;
	uint eip;
	uint eflags;
	uint eax,ecx,edx,ebx,esp,ebp,esi,edi;
	ushort es;
	ushort __reserved6;
	ushort cs;
	ushort __reserved7;
	ushort ss;
	ushort __reserved8;
	ushort ds;
	ushort __reserved9;
	ushort fs; 
	ushort __reserved10;
	ushort gs;
	ushort __reserved11;
	ushort ldt;
	ushort __reserved12;
	ushort T:1;
	ushort __reserved13:15;
	ushort iomap_addr;
} __attribute__((__packed__)) tss;

#endif
