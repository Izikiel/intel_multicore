#include <command_signatures.h>
#include <command.h>
#include <console.h>

uint64_t command_clear(uint32_t argc, char argv[][101]){	
	console_clear();
	return NORMAL_EXIT;
}