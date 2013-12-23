#include <command.h>
#include <utils.h>
#include <screen.h>

#define COMMAND_COUNT 1

command_binder commands[COMMAND_COUNT] = {
	[0] = 
	{
		.command_name = "clrscr",
		.command_method_ptr = ((uint64_t)(&clrscr))
	}
};

char* parseCommand(char* command){	
	//int i=0;
	//for(i=0;i<COMMAND_COUNT;i++){
	//	if(strcmp(command, commands[i].command_name)==0){
	//		commands[i].command_method_ptr();
	//	}
	//}

	return "Comando no reconocido";
}