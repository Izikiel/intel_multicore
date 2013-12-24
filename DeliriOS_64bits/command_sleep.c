#include <command_signatures.h>
#include <command.h>
#include <console.h>
#include <utils.h>
#include <timer.h>

uint64_t command_sleep(uint32_t argc, char argv[][101]){
	scrn_printf("(TODO: AJUSTAR timer.c y PIT FREQ)\n");
	if(argc == 2){
		uint64_t msec = atoi(argv[1]);
		scrn_printf("Sleep por %d milisegundos lanzado...\n", msec);
		sleep(msec);
		scrn_printf("Sleep por %d milisegundos finalizado\n", msec);
	}else{
		scrn_printf("Uso del comando sleep:\n");
		scrn_printf("sleep <tiempo de espera en milisegundos> (TODO: AJUSTAR timer.c y PIT FREQ)");
		return BAD_ARGUMENTS;
	}
	return 0;
}