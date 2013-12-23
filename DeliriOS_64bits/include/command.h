#ifndef __COMMAND_H__
#define __COMMAND_H__

#include <stdint.h>

typedef struct command_binder_str {
	char*		command_name;
	void*		command_method_ptr;
} __attribute__((__packed__)) command_binder;

char* parseCommand(char* command);

//comandos
char* command_paramtest(uint32_t argc, char** argv);
char* command_clrscr(uint32_t argc, char** argv);

#endif  /* !__COMMAND_H__ */
