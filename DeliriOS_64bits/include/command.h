#ifndef __COMMAND_H__
#define __COMMAND_H__

#include <stdint.h>

typedef struct command_binder_str {
	char*		command_name;
	void*		command_method_ptr;
} __attribute__((__packed__)) command_binder;

uint64_t parseCommand(char* command);

//return codes
#define NORMAL_EXIT 0
#define NOT_FOUND_COMMAND 1

//comandos
uint64_t command_paramtest(uint32_t argc, char argv[][101]);
uint64_t command_scrn_clear(uint32_t argc, char argv[][101]);

#endif  /* !__COMMAND_H__ */
