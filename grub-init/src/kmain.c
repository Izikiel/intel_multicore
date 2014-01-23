//kernel.c - Rutina de inicio del kernel del sistema operativo.
//Es llamada desde loader.asm

#include <multiboot.h>

//Simbolo de linker para el final del kernel. La direccion que contiene es
//un lugar despues de que termina el kernel. Esta definido en el linker script
//extern uint kernel_code_end;

//Simbolos de linker para el lugar donde empieza la pagina de codigo en 16
//bits para modo real de inicio de los Application Processors. Necesitamos
//saberla para habilitar multicore. Tambien buscamos la seccion donde inician
//y terminan los codigos 16 bits con los que linkeamos.

//Inicio de donde queremos el codigo de mp para APs
//extern ushort ap_startup_code_page;

//Encontrar un modulo dado su path. No estoy seguro de que tan portable
//es pero simplifica bastante la vida.
//module_t * module_by_path(const multiboot_info_t * mbd, const char * path){
//	module_t * modules = (module_t *) mbd->mods_addr;
//	for(uint i = 0; i < mbd->mods_count; i++){
//		char * str = (char *) modules[i].string;
//		//Asumo null-terminated el string
//		if(strcmp(str,path) == 0){
//			return &modules[i];
//		}
//	}	
//	kernel_panic("El modulo %s no pudo ser hallado",path);
//	return NULL;
//}

void kmain(const multiboot_info_t* mbd, unsigned long magic)
{
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		//scrn_setmode(GREEN,BLACK);
		//scrn_print("Algo salio muy muy mal. No se que mas decirte.");
		//return;
	}
	while(1);
}
