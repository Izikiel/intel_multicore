#include <multiboot.h>
#include <utils.h>

extern void* apStartupCode;
void* ap_startup_code_page = (void*)0x2000;

//Encontrar un modulo dado su path. No estoy seguro de que tan portable
//es pero simplifica bastante la vida.
module_t * module_by_path(const multiboot_info_t * mbd, const char * path){
	module_t * modules = (module_t *) mbd->mods_addr;
	for(uint32_t i = 0; i < mbd->mods_count; i++){
		char * str = (char *) modules[i].string;
		//Asumo null-terminated el string
		if(strcmp(str,path) == 0){
			return &modules[i];
		}
	}
	return NULL;
}

/*
En eax devuelvo el puntero a DeliriOs
En la variable externa devuelvo el puntero al stage2 de los ap
*/
void* kmain(const multiboot_info_t* mbd, unsigned long magic)
{
	//Chequeamos el magic number que viene de grub
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		//scrn_print("Algo salio muy muy mal. No se que mas decirte.");
		return NULL;
	}

	//Chequeamos que la cantidad de memoria RAM presente.
	if(mbd->flags & 1) {
		//scrn_printf("\tCantidad de RAM en el sistema:\n"
		//            "\t\tLower: %u Kb, Upper: %u Kb\n",
		//            mbd->mem_lower,mbd->mem_upper);
	} else {
		//scrn_printf("Mapa de memoria de GRUB invalido");
		return NULL;
	}

	//Hold on bitches, nasty code ahead !!

	//Explico esto con una gran sonrisa. Grub me cargo un "modulo" en alguna parte
	//Obtengo con esto la posicion de memoria donde lo cargo
	//Al ser un archivo binario plano, la primer instruccion es valida
	//entonces lo obtengo y hago un jmp violento a ese archivo, que inicia en modo
	//protegido y se encarga de levantar DeliriOS

	//...i told you it was nastier than hell :P
	module_t * deliriOSBinary = module_by_path(mbd, "/kernel64.bin64");
	if(deliriOSBinary == NULL){
		//scrn_print("Modulo no encontrado.");
		return NULL;
	}

	void * entryPoint = (void *) deliriOSBinary->mod_start;
	module_t * apFullCodeModule = module_by_path(mbd, "/ap_full_code.bin64");

	if (apFullCodeModule == NULL)
	{
		return NULL;
	}

	module_t * apStartupCodeModule = module_by_path(mbd, "/ap_startup_code");

	if(apStartupCodeModule == NULL){
		//scrn_print("Modulo no encontrado.");
		apStartupCode = NULL;
	}

	apStartupCode = (void *) apStartupCodeModule->mod_start;

	//Copiar el codigo de inicio de procesador AP a la direccion indicada
	//__asm __volatile("xchg %%bx, %%bx" : :);
	//__asm __volatile("xchg %%bx, %%bx" : :);
	memcpy(ap_startup_code_page, apStartupCode,
		apStartupCodeModule->mod_end - apStartupCodeModule->mod_start);
	// __asm __volatile("xchg %%bx, %%bx" : :);

	uint32_t* srch_pointer = (uint32_t*) ap_startup_code_page;

	for (int i = 0; i < 10 ; ++i)
	{
		if (srch_pointer[i] == 0xABBAABBA){
			srch_pointer[i] = apFullCodeModule->mod_start;
			// __asm __volatile("xchg %%bx, %%bx" : :);
			// __asm __volatile("xchg %%bx, %%bx" : :);
			break;
		}
	}

	return entryPoint;
}
