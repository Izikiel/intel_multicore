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
		.command_name = "clear\0",
		.command_method_ptr = &command_scrn_clear
	}
};

uint64_t parseCommand(char* command){
	//cuento cantidad de parametros <=> cantidad de espacios +1
	uint32_t paramCount = needleCount(command, ' ', 0) + 1;
	//armo buffer de parametros, doy una longuitud maxima de 100 caracteres por parametro
	char params[paramCount][101];
	//inicializo el buffer de parametros
	memset(params, '\0', paramCount*101);
	
	//parseo los parametros
	uint32_t currentParam=0;
	uint32_t lastParamIdx = 0;
	while(currentParam<paramCount){
		uint32_t baseIdx = lastParamIdx;
		lastParamIdx = nextTokenIdx(command, ' ', lastParamIdx + 1);
		
		//tengo el currentParam-esimo parametro en command[baseIdx..lastParamIdx-1]
		uint32_t bufferIdx=0;
		while(baseIdx<lastParamIdx){
			//scrn_putc(command[baseIdx], modoEscrituraTexto);
			params[currentParam][bufferIdx] = command[baseIdx];
			bufferIdx++;
			baseIdx++;
		}

		lastParamIdx++;//salteo el espacio
		currentParam++;
	}

	//en params[0..paramCount) tengo los parametros parseados

	int i=0;
	uint64_t (*commandPtr)(int, char argv[][101]);
	for(i=0;i<COMMAND_COUNT;i++){
		if(strcmp(params[0], commands[i].command_name) == 0){			
			commandPtr = commands[i].command_method_ptr;
			//add a new line to de console to indicate that we've processed the command
			scrn_printf("\n");
			return commandPtr(paramCount, params);
		}
	}
	return NOT_FOUND_COMMAND;
}

uint64_t command_paramtest(uint32_t argc, char argv[][101]){	
	scrn_printf("Numero de parametros leidos: %d\n", argc);
	scrn_printf("Parametros:\n");
	for(int i=0;i<argc;i++){
		scrn_printf("%s\n", argv[i]);
	}
	return NORMAL_EXIT;
}

uint64_t command_scrn_clear(uint32_t argc, char argv[][101]){
	scrn_clear();
	return NORMAL_EXIT;
}