#include <command_signatures.h>
#include <command.h>
#include <console.h>
#include <utils.h>
#include <timer.h>

uint64_t command_listcommands(const uint32_t argc, const char argv[][101]){	
	console_printf("Lista de comandos de DeliriOS:\n");

	uint64_t i = 0;
	uint64_t command_count = getCommandCount();
	while(i < command_count){
		console_printf("\t* %s: %s\n", getCommandName(i), getCommandDescription(i));
		i++;
	}
	return NORMAL_EXIT;
}