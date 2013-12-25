#ifndef __COMMAND_H__
#define __COMMAND_H__

#include <types.h>
#include <command_signatures.h>

typedef struct command_binder_str {
	char*		command_name;
	char*		command_description;
	void*		command_method_ptr;
} __attribute__((__packed__)) command_binder;

uint64_t parseCommand(char* command);
bool isRunningCommand();
const char* getCommandName(uint64_t command_idx);
const char* getCommandDescription(uint64_t command_idx);
uint64_t getCommandCount();

//return codes
#define NORMAL_EXIT 0
#define NOT_FOUND_COMMAND 1
#define BAD_ARGUMENTS 2

#endif  /* !__COMMAND_H__ */
