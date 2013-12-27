#include <kmain64.h>
#include <i386.h>
#include <console.h>
#include <mmu.h>
#include <timer.h>
#include <multicore.h>
#include <utils.h>

extern uint64_t start_section_text;
extern uint64_t end_section_text;
//extern uint64_t ap_startup_code_page;		 ya esta en multicore.h
extern uint64_t end_ap_startup_code_page;
extern uint64_t start_section_data;
extern uint64_t end_section_data;
extern uint64_t start_section_rodata;
extern uint64_t end_section_rodata;
extern uint64_t start_section_bss;
extern uint64_t end_section_bss;

void startKernel64(){

	//En este punto lo que se tiene inicializado es:
	// - Mapeados con Identity mapping los primeros 2 megas de memoria(con PAE) -> esto se hace en modo protegido de 32
	// 	 para poder pasar a modo largo sin problemas.
	// - IDT de 64 bits y ISR que captura las excepciones y los irq de reloj y teclado
	// - Callbacks de la ISR de reloj y teclado a contextManager
	// - Driver de pantalla que permite imprimir Strings y Numeros en forma abstracta
	// - Consola de comandos y parser de input
	// - interfaz para poder obtener todos los registros del CPU desde C

	//-----------------------------------------------------------------------------------------------------------------

	//inicializo la consola
	//pongo 5 porque es la linea donde termina de escribir kernel.asm con las macros de asm
	console_setYCursor(5);
	console_setXCursor(0);
	
	//armo la estructura de paginacion para hacer identitty mapping sobre los primeros 64 gb
	console_printf("Configuring paging...");
	
	init_64gb_identity_mapping(); //TODO: ESTA HARDCODEADO EN ASM!  ==> pasar a C mas bonitamente
	
	console_puts("OK!", greenOnBlack);
	console_printf("\n");

	console_printf("Configuring timer...");
	
	initialize_timer();
	
	console_puts("OK!", greenOnBlack);
	console_printf("\n");

	console_printf("Starting up multicore mode...\n");	
	console_printf("AP CPUs starting at RIP: %u\n", &ap_startup_code_page);
	multiprocessor_init();
	console_println("Multicore started OK!", greenOnBlack);

	//inicializar consola y dar bienvenida
	console_printf("\n\n");
	console_println("DeliriOS started up.", greenOnBlack);
	console_initialize_console();


	// - TODO: alinear la pila a 16 bytes en todos los calls a C desde asm!
	// - TODO: esta hardcodeado en asm lo de mapear los primeros 4 gb
	// - TODO: crear funciones en mmu para que sea posible mapear, desmapear paginas, y cambiar el contexto de paginas desde C

	//Disfrutar del tp final DeliriOS.
}

void kernel_panic(const char* functionSender, const char* message){
	console_set_panic_format();
	console_clear();
	console_printf("[KERNEL PANIC]: %s\n", message);

	console_printf("\nFuncion que produjo el error:\t%s\n", functionSender);

	console_printf("\nEstado de los registros:");
	console_printf("\nRAX = %u", getRAX());
	console_printf("\tRBX = %u", getRBX());
	console_printf("\nRCX = %u", getRCX());
	console_printf("\tRDX = %u", getRDX());
	console_printf("\nRSI = %u", getRSI());
	console_printf("\tRDI = %u", getRDI());
	console_printf("\nRBP = %u", getRBP());
	console_printf("\tRSP = %u", getRSP());
	console_printf("\nR8  = %u", getR8());
	console_printf("\tR9  = %u", getR9());
	console_printf("\nR10 = %u", getR10());
	console_printf("\tR11 = %u", getR11());
	console_printf("\nR12 = %u", getR12());
	console_printf("\tR13 = %u", getR13());
	console_printf("\nR14 = %u", getR14());
	console_printf("\tR15 = %u", getR15());
	console_printf("\nRIP = %u", getRIP());
	console_printf("\tCS  = %u", getCS());
	console_printf("\nDS  = %u", getDS());
	console_printf("\tES  = %u", getES());
	console_printf("\nFS  = %u", getFS());
	console_printf("\tGS  = %u", getGS());
	console_printf("\nSS  = %u", getSS());
	console_printf("\tCR0 = %u", getCR0());
	console_printf("\nCR2 = %u", getCR2());
	console_printf("\tCR3 = %u", getCR3());
	console_printf("\nCR4 = %u", getCR4());
	console_printf("\tRFLAGS = %u\n", getRFLAGS());

	//info de como linkea todo en memoria
	console_printf("section_text: 			[%u..%u)\n", &start_section_text, &end_section_text);
	console_printf("ap_startup_code_page: 	[%u..%u)\n", &ap_startup_code_page, &end_ap_startup_code_page);
	console_printf("section_data: 			[%u..%u)\n", &start_section_data, &end_section_data);
	console_printf("section_rodata: 		[%u..%u)\n", &start_section_rodata, &end_section_rodata);
	console_printf("section_bss: 			[%u..%u)\n", &start_section_bss, &end_section_bss);

	console_hide_text_cursor();
	haltCpu();
}
