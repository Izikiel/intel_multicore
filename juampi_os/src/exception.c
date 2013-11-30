#include <exception.h>
#include <scrn.h>
#include <irq.h>
#include <proc.h>

#define EXCEPTIONS 19
static char* msgs[]= {
	"DIVISION BY ZERO",
	"DEBUG",
	"NMI",
	"BREAKPOINT",
	"OVERFLOW",
	"BOUND",
	"INVALID OPCODE",
	"DEVICE NOT AVAILABLE",
	"DOUBLE FAULT",
	"COPROCESSOR SEGMENT OVERRUN",
	"INVALID TSS",
	"SEGMENT NOT PRESENT",
	"STACK FAULT",
	"GENERAL PROTECT",
	"PAGE FAULT",
	"FPU FLOATING-POINT ERROR",
	"ALIGNMENT CHECK",
	"MACHINE CHECK",
	"SIMD FLOATING POINT"
};

exception_handler exception_handlers[EXCEPTIONS] = { 0 };

//Registra un handler de excepcion nuevo
void register_exception_handler(exception_handler e, uint i)
{
	if(i < EXCEPTIONS) {
		exception_handlers[i] = e;
	}
}

//Inicializa los handlers de exception a defaultear a blue_screen
void initialize_exception_handlers()
{
	for(uint i = 0; i < EXCEPTIONS; i++) {
		register_exception_handler(blue_screen,i);
	}
}

//Handler default: imprime el estado de programa y muere
void blue_screen(exception_trace e)
{
	scrn_cls();
	scrn_setcursor(0,0);
	scrn_setmode(GREEN,BLACK);
	scrn_printf("EXCEPCION %s",msgs[e.excp_index]);
	scrn_print("\n\n\tHa ocurrido una excepcion del procesador.\n\tPor favor, tomese la situacion con mucha soda, picaron :).");
	scrn_print("\n\nESTADO DE LA MAQUINA:\n\n");
	scrn_printf("CR0= %u, CR2= %u, CR3= %u, CR4= %u\n\n",
	            e.ctrace.cr0,e.ctrace.cr2,e.ctrace.cr3,e.ctrace.cr4);
	scrn_printf("EAX= %u,EBX= %u,ECX= %u,EDX= %u,\nESI= %u, EDI= %u,ESP= %u, EBP= %u\n\n",
	            e.rtrace.eax,e.rtrace.ebx,e.rtrace.ecx,e.rtrace.edx,e.rtrace.esi,e.rtrace.edi,e.rtrace.esp,e.rtrace.ebp);
	scrn_printf("CS= %u, DS= %u, ES= %u,\nFS= %u, GS= %u, SS= %u\n\n",
	            e.strace.cs,e.strace.ds,e.strace.es,e.strace.fs,e.strace.gs,e.strace.ss);
	scrn_printf("ECODE= %u, EIP= %u, FLAGS= %u\n",e.error_code,e.itrace.eip,e.itrace.eflags);
	scrn_printf("TSS = %u",get_tr());
	while(1);
}
