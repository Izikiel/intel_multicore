#include <command.h>
#include <utils.h>
#include <screen.h>

#define COMMAND_COUNT 2

command_binder commands[COMMAND_COUNT] = {
	[0] = 
	{
		.command_name = "paramtest\0",
		.command_method_ptr = &command_paramtest
	},
	[1] = 
	{
		.command_name = "clrscr\0",
		.command_method_ptr = &command_clrscr
	}
};

char* parseCommand(char* command){
	int i=0;
	char* (*commandPtr)(int, char**);
	for(i=0;i<COMMAND_COUNT;i++){
		if(strcmp(command, commands[i].command_name) == 0){			
			commandPtr = commands[i].command_method_ptr;
			char* tmp[2] = {
				[0] = "hello", 
				[1] = "world"
			};
			return commandPtr(2, tmp);
		}
	}
	return "Comando no reconocido";
}

char* command_paramtest(uint32_t argc, char** argv){
	printLine("", modoEscrituraTexto);
	printLine("Parametros leidos", modoEscrituraTexto);
	for(int i=0;i<argc;i++){
		printLine(argv[i], modoEscrituraTexto);
	}
	return "Fin de los parametros";	
}

char* command_clrscr(uint32_t argc, char** argv){
	clrscr();
	return "";
}