#include <command_signatures.h>
#include <command.h>
#include <console.h>

uint64_t command_paramtest(const uint32_t argc, const char argv[][101]){	
	console_printf("Numero de parametros leidos: %d\n", argc);
	console_printf("Parametros:\n");
	for(int i=0;i<argc;i++){
		console_printf("%s\n", argv[i]);
	}
	return NORMAL_EXIT;
}