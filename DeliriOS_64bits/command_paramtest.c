#include <command_signatures.h>
#include <command.h>
#include <console.h>

uint64_t command_paramtest(uint32_t argc, char argv[][101]){
	scrn_printf("Numero de parametros leidos: %d\n", argc);
	scrn_printf("Parametros:\n");
	for(int i=0;i<argc;i++){
		scrn_printf("%s\n", argv[i]);
	}
	return NORMAL_EXIT;
}