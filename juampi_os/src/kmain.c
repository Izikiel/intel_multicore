//kernel.c - Rutina de inicio del kernel del sistema operativo.
//Es llamada desde loader.asm

#include <utils.h>
#include <gdt.h>
#include <scrn.h>
#include <idt.h>
#include <multiboot.h>
#include <irq.h>
#include <asserts.h>
#include <exception.h>
#include <timer.h>
#include <multicore.h>

//Simbolo de linker para el final del kernel. La direccion que contiene es
//un lugar despues de que termina el kernel. Esta definido en el linker script
extern uint kernel_code_end;

//Simbolos de linker para el lugar donde empieza la pagina de codigo en 16
//bits para modo real de inicio de los Application Processors. Necesitamos
//saberla para habilitar multicore. Tambien buscamos la seccion donde inician
//y terminan los codigos 16 bits con los que linkeamos.

//Inicio de donde queremos el codigo de mp para APs
extern ushort ap_startup_code_page;

//Encontrar un modulo dado su path. No estoy seguro de que tan portable
//es pero simplifica bastante la vida.
module_t * module_by_path(const multiboot_info_t * mbd, const char * path){
	module_t * modules = (module_t *) mbd->mods_addr;
	for(uint i = 0; i < mbd->mods_count; i++){
		char * str = (char *) modules[i].string;
		//Asumo null-terminated el string
		if(strcmp(str,path) == 0){
			return &modules[i];
		}
	}	
	kernel_panic("El modulo %s no pudo ser hallado",path);
	return NULL;
}

void kmain(const multiboot_info_t* mbd, unsigned long magic)
{
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		scrn_setmode(GREEN,BLACK);
		scrn_print("Algo salio muy muy mal. No se que mas decirte.");
		return;
	}
	scrn_cls();
	scrn_setmode(GREEN,BLACK);
	scrn_print("BIENVENIDO A juampiOS\n\t"
	           "Estamos trabajando para ofrecerle "
	           "el OS del futuro.\n");
	scrn_print("INICIALIZANDO GDT...");
	gdt_init();
	gdt_flush();

	scrn_print("OK\nINICIALIZANDO IDT PARA LAS EXCEPCIONES...");

	initialize_exception_handlers();
	idt_init_exceptions();
	remap_pic();

	scrn_print("OK\nINICIALIZANDO IDT PARA LAS INTERRUPCIONES Y SYSCALLS...");

	irq_init_handlers();
	
	//Frecuencia de 1 interrupccion cada 10 microsegundos
	//Esto se usa en el codigo de inicializacion de APs porque hay delays
	//necesarios para las IPIs.
	init_timer(25000);

	idt_init_interrupts();
	idt_flush();
	irq_sti_force();

	scrn_printf("OK\nCHEQUEANDO ESTADO DE LOS MODULOS...");
	scrn_printf("%u MODULOS CARGADOS\n",mbd->mods_count);

	scrn_print("CHECKEANDO ESTADO DE LA MEMORIA\n");

	//Chequeamos que la cantidad de memoria RAM presente.
	if(mbd->flags & 1) {
		scrn_printf("\tCantidad de RAM en el sistema:\n"
		            "\t\tLower: %u Kb, Upper: %u Kb\n",
		            mbd->mem_lower,mbd->mem_upper);
	} else {
		kernel_panic("Mapa de memoria de GRUB invalido");
	}

	scrn_print("INICIALIZANDO LAS ESTRUCTURAS DE MEMORIA DEL KERNEL...\n");
	//module_t* grub_modules = (module_t*) mbd->mods_addr;
	//uint kernel_end_addr = grub_modules[mbd->mods_count-1].mod_end;
	
	//El mapa de memoria upper es a partir del primer megabyte ergo el primer
	//lugar donde nos vamos de largo es 1024 kilobytes mas la memoria que dice GRUB
	scrn_printf("El kernel termina en %u\n",(uint)&kernel_code_end);
	scrn_printf("PROBANDO MODULOS DE KERNEL\n");

	char buffer[32];
	memset(buffer,0,sizeof(buffer));

	module_t * manifesto = module_by_path(mbd,"/manifesto.txt");
	char * mod_start = (char *) manifesto->mod_start;
	char * mod_end = (char *) manifesto->mod_end;
	memcpy(buffer,mod_start,(uint)(mod_end-mod_start));
	scrn_printf("\nEl modulo contiene: %s\n",buffer);

	
	scrn_printf("\nIniciando procesadores...\n");
	scrn_printf("\nEl codigo de inicio de APs esta en: %u\n",
		(uint)&ap_startup_code_page);

	module_t * aps = module_by_path(mbd,"/ap_startup_code");
	scrn_printf("Posicion del modulo: %u\n",aps->mod_start);

	//Copiar el codigo de inicio de procesador MP a la direccion indicada
	memcpy(&ap_startup_code_page,(void *)aps->mod_start,
		aps->mod_end - aps->mod_start);

	multiprocessor_init();
	while(1);
}
