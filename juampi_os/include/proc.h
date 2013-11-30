#ifndef __PROC_H
#define __PROC_H

#include <types.h>

//Estructura con los registros de proposito general
typedef struct{
	uint edi,esi,ebp,esp,ebx,edx,ecx,eax;
} gen_regs;

//Traza de interrupccion
typedef struct{
	uint eip,cs,eflags,useresp,ss;
} __attribute__((__packed__)) int_trace;

//Registros de control
typedef struct{
	uint cr0,cr2,cr3,cr4;
} ctrl_regs;

//Registros de selectores de segmento
typedef struct{
	uint cs,ds,es,fs,gs,ss;
} sel_regs;

typedef struct {
	uint excp_index;
	ctrl_regs ctrace;
	sel_regs strace;
	gen_regs rtrace;
	uint error_code;
	int_trace itrace;	
} exception_trace;

//Setter y getter para la task register (tr)
short get_tr();
void set_tr(short);

short get_cs();
uint get_eflags();

#endif
