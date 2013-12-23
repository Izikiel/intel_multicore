#ifndef __COMMAND_H__
#define __COMMAND_H__

#include <stdint.h>

typedef struct command_binder_str {
	char*		command_name;
	uint64_t	command_method_ptr;
} __attribute__((__packed__)) command_binder;

char* parseCommand(char* command);

#endif  /* !__COMMAND_H__ */
