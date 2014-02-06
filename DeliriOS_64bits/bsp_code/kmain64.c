#include <kmain64.h>
#include <console.h>
#include <i386.h>
#include <mmu.h>
#include <timer.h>
#include <multicore.h>
#include <utils.h>
#include <asserts.h>

extern uint64_t start_section_text;
extern uint64_t end_section_text;
extern uint64_t start_section_data;
extern uint64_t end_section_data;
extern uint64_t start_section_rodata;
extern uint64_t end_section_rodata;
extern uint64_t start_section_bss;
extern uint64_t end_section_bss;

void startKernel64_BSPMODE(){
	//breakpoint en entry point de bsp
	//breakpoint();
}

void startKernel64_APMODE(){
	//breakpoint en entry point de ap
	//breakpoint();
	//estan deshabilitadas las interrupciones enmascarables!
	//habilito interrupciones
	//irq_sti();
}

void kernel_panic(const char* functionSender, const uint64_t lineError, const char* fileError, const char* message){
		__asm __volatile("xchg %%bx, %%bx" : :);
//	    console_set_panic_format();
//        console_clear();
//        console_printf("[KERNEL PANIC]: %s\n", message);
//
//        console_printf("\nFuncion que produjo el error:\t%s\n", functionSender);
//
//        console_printf("\nEstado de los registros:");
//        console_printf("\nRAX = %u", getRAX());
//        console_printf("\tRBX = %u", getRBX());
//        console_printf("\nRCX = %u", getRCX());
//        console_printf("\tRDX = %u", getRDX());
//        console_printf("\nRSI = %u", getRSI());
//        console_printf("\tRDI = %u", getRDI());
//        console_printf("\nRBP = %u", getRBP());
//        console_printf("\tRSP = %u", getRSP());
//        console_printf("\nR8 = %u", getR8());
//        console_printf("\tR9 = %u", getR9());
//        console_printf("\nR10 = %u", getR10());
//        console_printf("\tR11 = %u", getR11());
//        console_printf("\nR12 = %u", getR12());
//        console_printf("\tR13 = %u", getR13());
//        console_printf("\nR14 = %u", getR14());
//        console_printf("\tR15 = %u", getR15());
//        console_printf("\nRIP = %u", getRIP());
//        console_printf("\tCS = %u", getCS());
//        console_printf("\nDS = %u", getDS());
//        console_printf("\tES = %u", getES());
//        console_printf("\nFS = %u", getFS());
//        console_printf("\tGS = %u", getGS());
//        console_printf("\nSS = %u", getSS());
//        console_printf("\tCR0 = %u", getCR0());
//        console_printf("\nCR2 = %u", getCR2());
//        console_printf("\tCR3 = %u", getCR3());
//        console_printf("\nCR4 = %u", getCR4());
//        console_printf("\tRFLAGS = %u\n", getRFLAGS());
//
//        //info de como linkea todo en memoria
//        console_printf("section_text:                         [%u..%u)\n", &start_section_text, &end_section_text);
//        console_printf("ap_startup_code_page:         [%u..%u)\n", &ap_startup_code_page, &end_ap_startup_code_page);
//        console_printf("section_data:                         [%u..%u)\n", &start_section_data, &end_section_data);
//        console_printf("section_rodata:                 [%u..%u)\n", &start_section_rodata, &end_section_rodata);
//        console_printf("section_bss:                         [%u..%u)\n", &start_section_bss, &end_section_bss);
//
	    //console_hide_text_cursor();
	    haltCpu();
}
