#include <kmain64.h>
#include <i386.h>
#include <mmu.h>
#include <timer.h>
#include <multicore.h>
#include <utils.h>
#include <asserts.h>

extern uint64_t start_section_text;
extern uint64_t end_section_text;
extern uint64_t end_ap_startup_code_page;
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
	breakpoint();
	haltCpu();
}
