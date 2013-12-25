#include <command.h>
#include <utils.h>
#include <console.h>

volatile bool isRunningCommandFlag = false;

#define COMMAND_COUNT 4

command_binder commands[COMMAND_COUNT] = {
	[0] = 
	{
		.command_name = "listcommands\0",
		.command_description = "Lista los comandos disponibles",
		.command_method_ptr = &command_listcommands,
		.command_priv = KERNEL_PRIVILEGE
	},
	[1] = 
	{
		.command_name = "paramtest\0",
		.command_description = "Testea parametros pasados por espacios",
		.command_method_ptr = &command_paramtest,
		.command_priv = KERNEL_PRIVILEGE
	},
	[2] = 
	{
		.command_name = "clear\0",
		.command_description = "Limpia la pantalla",
		.command_method_ptr = &command_clear,
		.command_priv = KERNEL_PRIVILEGE
	},
	[3] = 
	{
		.command_name = "sleep\0",
		.command_description = "Espera una cantidad de tiempo en milisegundos pasada por parametro",
		.command_method_ptr = &command_sleep,
		.command_priv = KERNEL_PRIVILEGE
	}
};

const char* getCommandName(uint64_t command_idx){
	return commands[command_idx].command_name;
}

const char* getCommandDescription(uint64_t command_idx){
	return commands[command_idx].command_description;	
}

uint64_t getCommandCount(){
	return COMMAND_COUNT;
}

bool isRunningCommand(){
	return isRunningCommandFlag;
}

command_result parseCommand(char* command){
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
			//console_putc(command[baseIdx], modoEscrituraTexto);
			params[currentParam][bufferIdx] = command[baseIdx];
			bufferIdx++;
			baseIdx++;
		}

		lastParamIdx++;//salteo el espacio
		currentParam++;
	}

	//en params[0..paramCount) tengo los parametros parseados

	int i=0;
	command_result (*commandPtr)(int, char argv[][101]);
	for(i=0;i<COMMAND_COUNT;i++){
		if(strcmp(params[0], commands[i].command_name) == 0){			
			commandPtr = commands[i].command_method_ptr;
			//add a new line to de console to indicate that we've processed the command
			console_printf("\n\tEjecutando comando '%s'...por favor aguarde\n\n", params[0]);
			isRunningCommandFlag = true;

			command_result resCode = INVALID_PRIVILEGE;
			switch(commands[i].command_priv){
				case KERNEL_PRIVILEGE:
					resCode = commandPtr(paramCount, params);
					break;
				case USER_PRIVILEGE:
					resCode = executeAsUser(commands[i], paramCount, params);					
					break;
				default:
					resCode = INVALID_PRIVILEGE;
					break;
			}

			isRunningCommandFlag = false;
			return resCode;
		}
	}
	return NOT_FOUND_COMMAND;
}

command_result executeAsUser(command_binder commandToExecute, int argc, char argv[][101]){
	command_result (*commandPtr)(int, char argv[][101]);
	commandPtr = commandToExecute.command_method_ptr;

	//TODO: hacer el framework para tss y jumpear a gdt DPL 3

	command_result resCode = commandPtr(argc, argv);
	return resCode;
}