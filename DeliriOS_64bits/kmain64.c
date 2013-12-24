#include <kmain64.h>
#include <i386.h>
#include <screen.h>
#include <mmu.h>
#include <timer.h>

void startKernel64(){
	initialize_timer(1000);//TODO no es el valor real!


	//En este punto lo que se tiene inicializado es:
	// - Mapeados con Identity mapping los primeros 2 megas de memoria(con PAE) -> esto se hace en modo protegido de 32
	// 	 para poder pasar a modo largo sin problemas.
	// - IDT de 64 bits y ISR que captura las excepciones y los irq de reloj y teclado
	// - Callbacks de la ISR de reloj y teclado a contextManager
	// - Driver de pantalla que permite imprimir Strings y Numeros en forma abstracta
	// - Consola de comandos y parser de input
	// - interfaz para poder obtener todos los registros del CPU desde C

	//-----------------------------------------------------------------------------------------------------------------

	//armo la estructura de paginacion para hacer identitty mapping sobre los primeros 64 gb
	init_64gb_identity_mapping();
	
	//inicializar timer, y contexto inicial parecido al juampios
	//...

	//inicializar multicore
	//...

	//inicializo la consola
	//pongo 5 porque es la linea donde termina de escribir kernel.asm con las macros de asm
	scrn_setYCursor(5);
	scrn_setXCursor(0);
	scrn_println("DeliriOS iniciado.", redOnBlack);
	scrn_println("--------------------------------------------------------------------------------", modoEscrituraTexto);
	scrn_initialize_console();

	sleep(10);
	
	// Tests de printf
	//scrn_printf("Hola mundo:\t %u %s\n", 123, "jojojo");
	//scrn_printf("Hola mundo:\t %u %s\n\r%s", 123, "jojojo", "paramtest 1 2 3");


	// - TODO: alinear la pila a 16 bytes en todos los calls a C desde asm!
	// - TODO: poner de forma correcta el callback a notificarExcepcion en isr.asm (convencion C de 64 bits!)
	// - TODO: crear funciones en mmu para que sea posible mapear, desmapear paginas, y cambiar el contexto de paginas desde C
	// - TODO: incorporar multicore.c. La principal complicacion es que esta muy arraigado al juampiOS de 32 bits.
	//	Deberiamos recrear un contexto inicial parecido pero en 64 bits, por lo pronto ya esta hecha la lib de pantalla de forma similar.
	// BUG: la consola solo le envia al parser la ultima linea escrita ( si escribimos mas de 80 caracteres y damos enter solo se toman los de la ultima linea )


	//Disfrutar del tp final DeliriOS.
}

void kernel_panic(char* message){
	scrn_clear();
	scrn_println(message, whiteOnBlue);
	/*
		TODO: Completar info de los registros de i386.h
	*/
	scrn_hide_text_cursor();
	haltCpu();
}