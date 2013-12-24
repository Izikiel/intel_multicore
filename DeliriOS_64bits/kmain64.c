#include <kmain64.h>
#include <i386.h>
#include <screen.h>
#include <mmu.h>
#include <timer.h>
#include <multicore.h>

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
	scrn_setYCursor(5);
	scrn_setXCursor(0);
	
	//armo la estructura de paginacion para hacer identitty mapping sobre los primeros 64 gb
	scrn_printf("Configuring paging...");
	
	init_64gb_identity_mapping();
	
	scrn_puts("Ok!", greenOnBlack);
	scrn_printf("\n");

	scrn_printf("Configuring timer...");
	
	initialize_timer();
	
	scrn_puts("Ok!", greenOnBlack);
	scrn_printf("\n");

	scrn_printf("Starting up multicore mode...");
	
	//inicializar multicore
	multiprocessor_init();
	
	scrn_puts("Ok!", greenOnBlack);
	scrn_printf("\n");
	scrn_println("DeliriOS started up.", redOnBlack);
	scrn_println("--------------------------------------------------------------------------------", modoEscrituraTexto);
	scrn_initialize_console();

	//tests de sleep
	//uint64_t ticks = 20;
	//scrn_printf("Waiting %d ticks\n", ticks);
	//sleep(ticks);
	//scrn_printf("Waiting %d ticks finished\n", ticks);
	
	// Tests de printf
	//scrn_printf("Hola mundo:\t %u %s\n", 123, "jojojo");
	//scrn_printf("Hola mundo:\t %u %s\n\r%s", 123, "jojojo", "paramtest 1 2 3");


	// - TODO: alinear la pila a 16 bytes en todos los calls a C desde asm!
	// - TODO: crear funciones en mmu para que sea posible mapear, desmapear paginas, y cambiar el contexto de paginas desde C
	// BUG: la consola solo le envia al parser la ultima linea escrita ( si escribimos mas de 80 caracteres y damos enter solo se toman los de la ultima linea )


	//Disfrutar del tp final DeliriOS.
}

void kernel_panic(char* message){
	//scrn_clear();
	scrn_println(message, whiteOnBlue);
	/*
		TODO: Completar info de los registros de i386.h
	*/
	scrn_hide_text_cursor();
	haltCpu();
}