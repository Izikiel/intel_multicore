#ifndef __COMMAND_H__
#define __COMMAND_H__

#include <types.h>
#include <command_signatures.h>

typedef enum command_result {
		NORMAL_EXIT = 0,
		NOT_FOUND_COMMAND = 1,
		BAD_ARGUMENTS = 2,
		INVALID_PRIVILEGE = 3
	} command_result;


typedef enum command_privilege {
		KERNEL_PRIVILEGE = 0,
		USER_PRIVILEGE = 3
	} command_privilege;

typedef struct command_binder_str {
	const char*				command_name;
	const char*				command_description;
	const void*				command_method_ptr;
	const command_privilege	command_priv;
} __attribute__((__packed__)) command_binder;

command_result parseCommand(char* command);
bool isRunningCommand();
const char* getCommandName(uint64_t command_idx);
const char* getCommandDescription(uint64_t command_idx);
uint64_t getCommandCount();

command_result executeAsUser(command_binder commandToExecute, int argc, char argv[][101]);

#endif  /* !__COMMAND_H__ */
