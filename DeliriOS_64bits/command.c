#include <command.h>
#include <utils.h>
#include <screen.h>

#define COMMAND_COUNT 1

command_binder commands[COMMAND_COUNT] = {
	[0] = 
	{
		.command_name = "clrscr\0",
		.command_method_ptr = ((uint64_t)(&clrscr))
	}
};

char* parseCommand(char* command){
	int i=0;
	for(i=0;i<COMMAND_COUNT;i++){
		if(strcmp(command, commands[i].command_name) == 0){
			return command;
		}
	}

	return "Comando no reconocido";	
}