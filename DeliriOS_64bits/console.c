#include <console.h>
#include <command.h>
#include <defines.h>
#include <utils.h>
#include <vargs.h>
#include <asserts.h>
#include <i386.h>

//NOTA:ojo que como es puntero a short, incrementar el puntero corre solo sizeof(uint16_t) = 2 bytes
static uint16_t* _outputMemoryPtr = (uint16_t*) VIDEO_MEMORY;

//Indicador de ultima linea impresa, se usa como flag para shiftear
static uint32_t currentLine = 0;
static uint32_t currentCol = 0;

//Indicador de posicion del cursor
static uint32_t cursorLine = 0;
static uint32_t cursorCol = VIDEO_COLS-1;

//variables para impresion de cursor
static char* cursorBuffer = "\\-/|";
static uint32_t cursorIndex = 0;
static uint32_t cursorBufferSize = 4;

//flag de kernel_panic
static bool panicFormat = false;

//formato personalizable en printf
static uint8_t formatoPrintf = modoEscrituraTexto;

//para levantar mas de una linea cuando leo de la pantalla un comando
static uint64_t columnOverflowLineOffset=0;

//-------------- Start Command Processing functions -------------------

void console_processEnterKey(){
	if(isRunningCommand() != true){
		char buffer[VIDEO_COLS+1];
		//quitar el cursor de la pantalla
	    console_hide_text_cursor();
	    //inicializar buffer a null
	    memset(buffer, '\0', VIDEO_COLS+1);
	    //load buffer with wrote line
	    console_get_last_line(buffer);
	    //parse command and get result
	    command_result commandResult = parseCommand(buffer);
	    
	    switch(commandResult){
	        case NORMAL_EXIT:
	            break;
	        case NOT_FOUND_COMMAND:
	            console_printf("\nComando no encontrado\n");
	            break;
	        case BAD_ARGUMENTS:
	            console_printf("\nArgumentos erroneos\n");
	            break;
	        case INVALID_PRIVILEGE:
	            console_printf("\nPrivilegios erroneos\n");
	            break;
	        default:
	            console_printf("\n[Resultado Erroneo] Codigo de error: %d\n", commandResult);
	            break;
	    }
	    //print command symbol
	    console_initialize_console(); 
    }//else{
        //console_printf("Hay un comando corriendo...por favor espere que finalice\n");
    //}
}

void console_processTabKey(){
	if(isRunningCommand() != true){
		//4 espacios
		console_putc(' ', modoEscrituraTexto);
		console_putc(' ', modoEscrituraTexto);
		console_putc(' ', modoEscrituraTexto);
		console_putc(' ', modoEscrituraTexto);   
    }//else{
    //    console_printf("Hay un comando corriendo...por favor espere que finalice\n");
    //}
}

void console_processBackSpaceKey(){
	if(isRunningCommand() != true){
		console_moveBack(false/*not calling from system*/);
    }//else{
    //    console_printf("Hay un comando corriendo...por favor espere que finalice\n");
    //}
}

void console_processKey(const char keyPressed){
	if(isRunningCommand() != true){
		console_putc(keyPressed, modoEscrituraTexto);
    }//else{
    //    console_printf("Hay un comando corriendo...por favor espere que finalice\n");
    //}
}


//-------------- End Command Processing functions -------------------

//-------------- Start Config functions -------------------

void console_setYCursor(uint32_t number){
	currentLine=number;
}

void console_setXCursor(uint32_t number){
	currentCol=number;
}

void console_set_panic_format(){
	panicFormat = true;
}

void console_update_text_cursor(){
	cursorLine=currentLine;
	cursorCol=currentCol;
	console_pos_putc(' ', whiteOnGreen, currentCol, currentLine);
}

void console_hide_text_cursor(){
	console_pos_putc(' ', (panicFormat == true ? modoEscrituraBSOD : modoEscrituraTexto), currentCol, currentLine);
}

void console_initialize_console()
{
	console_putc('$', modoEscrituraSymbol);
    console_putc('>', modoEscrituraSymbol);
    console_putc(' ', modoEscrituraSymbol);
}

void console_reset_console()
{
	console_clear();
	console_putc('$', modoEscrituraSymbol);
    console_putc('>', modoEscrituraSymbol);
    console_putc(' ', modoEscrituraSymbol);
}

//-------------- End Config functions -------------------

//-------------- Start Console movement functions -------------------

void console_moveUp(){
	//backup del buffer de pantalla actual
	uint16_t tmpBuffer[VIDEO_FILS][VIDEO_COLS];
    //copio el buffer de video al buffer temporal
    memcpy(tmpBuffer, _outputMemoryPtr, sizeof(tmpBuffer));
    //copio el buffer a pantalla a partir de la segunda linea
    memcpy(_outputMemoryPtr, tmpBuffer[1], VIDEO_COLS*(VIDEO_FILS-1)*sizeof(uint16_t));        
    //limpio ultima linea
    memset(_outputMemoryPtr + (VIDEO_FILS-1)*VIDEO_COLS, 0, VIDEO_COLS*sizeof(uint16_t));    
}

void console_moveBack(bool fromSystem){
	uint16_t limitBackSpace = 0;
	if(fromSystem == false){
		limitBackSpace = 3;
	}
	if(currentCol>limitBackSpace){
		currentCol--;
	}
	//por ahora no permito borrar lineas de mas arriba de la actual!
	/*else{
		currentCol=VIDEO_COLS-1;
		currentLine--;
	}*/
	console_pos_putc(' ', modoEscrituraTexto, currentCol, currentLine);

	//guardo la posicion del cursor actual
	uint32_t cline = cursorLine;
	uint32_t ccol = cursorCol;

	//pongo la posicion del proximo cursor a ser escrito en pantalla
	cursorLine=currentLine;
	cursorCol=currentCol;

	//limpio cursor
	console_pos_putc(' ', modoEscrituraTexto, ccol, cline);
	//pongo cursor nuevo
	console_pos_putc(' ', whiteOnGreen, currentCol, currentLine);	
}

void console_clear(){
	uint8_t _modoEscritura = (panicFormat == true ? modoEscrituraBSOD : modoEscrituraTexto);//fondo negro, letras blancas
	uint8_t _caracter = ' ';//espacio en blanco

	uint32_t offset = 0;
	while(offset<VIDEO_FILS*VIDEO_COLS)
	{
		uint16_t pixel = (_modoEscritura << 8) | _caracter;
		*(_outputMemoryPtr + offset) = pixel;
		offset++;
	}
	currentLine=0;
	currentCol=0;
}

//-------------- End Console movement functions -------------------

//-------------- Start Console clock cursor functions -------------------

//es llamada periodicamente por el reloj del sistema
void console_print_next_cursor(){
	//imprime el cursor en la ultima linea de pantalla
	console_pos_putc(cursorBuffer[cursorIndex], whiteOnBlue, VIDEO_COLS-1, 0);
	cursorIndex = (cursorIndex+1) % cursorBufferSize;
}

//-------------- End Console clock cursor functions -------------------

//-------------- Start Console FORMATTED printing functions -------------------

// Tomado de juampi OS
void console_printf(const char* msg, ...)
{
        va_list l;
        va_start(l,msg);
        console_vprintf(msg, l);
        va_end(l);
}

void console_printf_change_format(uint8_t format){
	formatoPrintf = format;
}

// Tomado de juampi OS
void console_vprintf(const char* msg, va_list l)
{
		uint16_t outputFormat = (panicFormat == true ? modoEscrituraBSOD : formatoPrintf);
        uint64_t i;
        char buffer[64];
        for(i = 0; msg[i]; i++) {
                switch(msg[i]) {
                case '%':
                        i++;
                        switch(msg[i]) {
                        case '%':
                                console_putc(msg[i], outputFormat);
                                break;
                        case 'u':
                        		decToHexStr(va_arg(l, uint64_t), buffer, ""/*no title*/, 1/*print 0x prefix*/);
                                console_puts(buffer, outputFormat);
                                break;
                        case 'd':
                                itoa(va_arg(l, uint64_t), buffer);                                
                                console_puts(buffer, outputFormat);
                                break;
                        case 's':
                                console_puts(va_arg(l, char*), outputFormat);
                                break;
                        case 'c':
                                console_putc(va_arg(l, uint64_t), outputFormat);
                                break;
                        case 'b':
                        		console_puts(va_arg(l, uint64_t) ? "true" : "false", outputFormat);
                                break;
                        }
                        break;
                default:
                        console_putc(msg[i], outputFormat);
                        break;
                }
        }
}

//-------------- End Console FORMATTED printing functions -------------------

//-------------- Start Console printing functions -------------------

void console_println(const char* cadena, uint8_t format){
	if(currentLine<VIDEO_FILS-1)
	{
		//si estoy dentro de la primer pantalla sin shiftear imprimo normalmente
		console_pos_print(cadena, format, 0, currentLine);
		currentLine++;
	}
	else
	{
		//si me excedi de las lineas, shifteo la pantalla una linea hacia arriba
		console_moveUp();
		//escribo en la anteultima linea de la pantalla
		console_pos_print(cadena, format, 0, VIDEO_FILS-2);//notar que los indices x,y comienzan en cero por eso se le resta uno a VIDEO_FILS
	}
	currentCol=0;

	//pongo la posicion del proximo char a ser escrito en pantalla
	console_update_text_cursor();
}

//como no existe la sobrecarga de funciones, me armo otra funcion para imprimir numeros.
void console_printlnNumber(uint32_t number, uint8_t format){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	memset(buffer, '\0', VIDEO_FILS*VIDEO_COLS);
	itoa(number, buffer);
	console_println(buffer, format);
}

void console_putc(char caracter, uint8_t format){
	//process console movement
	if(currentCol == VIDEO_COLS){
		//Linea nueva		
        console_println("", format);
        currentCol=0;
        columnOverflowLineOffset++;
	}

	//process \n, \t, etc special chars
	switch(caracter){
		case '\n': //Linea nueva
			//quitar el cursor de la pantalla
            console_hide_text_cursor();
	        console_println("", format);
			//quitar el cursor de la pantalla
            console_hide_text_cursor();
	        currentCol=0;
	        columnOverflowLineOffset=0;
			break;
		case '\r': //Borrar linea actual
	        while(currentCol>0){
	        	console_moveBack(false/*not calling from system*/);;
	        }
			break;
		case '\t': //Tab
            //4 espacios
            console_putc(' ', format);
            console_putc(' ', format);
            console_putc(' ', format);
            console_putc(' ', format);  
			//quitar el cursor de la pantalla
            console_hide_text_cursor();            
			break;
		default:
			fail_unless(currentCol<80);
			fail_unless(currentLine<25);
			console_pos_putc(caracter, format, currentCol, currentLine);
			//avanzar columna
			currentCol++;
			break;
	}

	//pongo la posicion del proximo char a ser escrito en pantalla
	console_update_text_cursor();
}

void console_puts(const char* string, uint8_t format)
{
	uint32_t len = strlen(string);
	uint32_t i = 0;
	while(i<len){
		console_putc(string[i], format);
		i++;
	}
}
//-------------- End Console printing functions -------------------

//-------------- Start Console POSITIONED printing functions -------------------

void console_pos_putc(char caracter, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint16_t offset = posX + posY * VIDEO_COLS;	
	uint16_t pixel = (format << 8) | caracter;
	if(offset<4000){
		setRSP(offset);
		breakpoint();			
	}
	fail_unless(offset<4000);
	*(_outputMemoryPtr + offset) = pixel;
}

void console_pos_print(const char* cadena, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint32_t strlength = strlen(cadena);
	uint8_t idx = 0;
	while(idx < strlength)
	{
		if(posX + idx > VIDEO_COLS){
			posY++;
			posX=0;
		}
		console_pos_putc(cadena[idx], format, posX + idx, posY);
		idx++;
	}
}

void console_pos_printInt(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	itoa(number, buffer);
	console_pos_print(buffer, format, posX, posY);
}

//-------------- End Console POSITIONED printing functions -------------------

//-------------- Start Console reading functions -------------------

void console_get_last_line(char* buffer)//Nota, el buffer devuelto es de tamanio VIDEO_COLS
{
	//voy a leer a partir de la ultima linea escrita la cantidad de caracteres indicada por currentCol, 
	//salteandome los caracteres de formato y salteandome los primeros 3 chars del simbolo del sistema

	//Nota: usando columnOverflowLineOffset tengo la cantidad de lineas a leer(por si escribimos mas de una linea y se hace
	//overflow y salto de linea.
	
	uint16_t offsetStart = (currentLine - columnOverflowLineOffset) * VIDEO_COLS  + 3/*padding symbol system*/;
	uint16_t offsetEnd = currentLine*VIDEO_COLS + currentCol;
	
	uint64_t screenIdx = offsetStart;
	uint64_t idxBuffer = 0;
	while(screenIdx < offsetEnd){
		uint16_t readFormattedChar = *(_outputMemoryPtr + screenIdx);
		char readChar = (uint8_t)readFormattedChar;
		buffer[idxBuffer] = readChar;
		idxBuffer++;
		screenIdx++;
	}
}

//-------------- End Console reading functions -------------------

//-------------- Start Console FORMATTED reading functions -------------------

//hay que pasarle PUNTEROS a lo que queremos levantar!
void console_scanf(const char* format, ...){
	char buffer[256];
	memset(buffer, '\0', 256);
	console_get_last_line(buffer);
	va_list l;
	va_start(l, format);	
	console_sscanf(buffer, format, l);
	va_end(l);
}

//buffer es de 256 bytes
//devuelve el currentIdx modificado luego del parseo
uint64_t parseNextString(const char* msg, char* buffer, char nextDelimiter, uint64_t currentIdx){
//	console_printf("\t'%s' hasta '%c'\n", msg, nextDelimiter);
	memset(buffer, '\0', 256);
	uint16_t j=0;
	//itero hasta encontrar un espacio o otro parametro que comience con % o el fin de cadena
	while((msg[currentIdx] != nextDelimiter) && (msg[currentIdx] != '\0')){
		buffer[j++]=msg[currentIdx++];
		fail_if(j>=256);//buffer overflow(es de 256)
	}
	
	if(msg[currentIdx] == nextDelimiter){
		//ignoramos el delimitador
		currentIdx++;
	}
	
	return currentIdx;
}

void console_sscanf(const char* inputStr, const char* format, va_list l)
{
//	console_printf("Input string: '%s'\n", inputStr);
//	console_printf("Format string: '%s'\n", format);
	char buffer[256];//numeros y cadenas de hasta 256 digitos, sino volamos en pedazos
	uint64_t formatIdx = 0;
	uint64_t inputStrIdx = 0;
	bool continueParsing = format[formatIdx] != '\0';

	while(continueParsing){
		
		if(format[formatIdx] == '%'){
			
			formatIdx++;

			if(format[formatIdx] != '\0'){

				switch(format[formatIdx]){
					case 'd'://parse integer						
						inputStrIdx = parseNextString(inputStr, buffer, format[formatIdx+1], inputStrIdx);
                		//ahora tengo en buffer capturado el "entero", lo guardo como numero en el ptr indicado
                		uint64_t* ptrInt = va_arg(l, uint64_t*);
                        *ptrInt = atoi(buffer);
						break;
                	case 's'://parse string
                		inputStrIdx = parseNextString(inputStr, buffer, format[formatIdx+1], inputStrIdx);
                		//ahora tengo en buffer capturado el "string", lo guardo como numero en el ptr indicado
                		char* ptrStr = va_arg(l, char*);
                		//lo copio, sino cuando salgo de la funcion lo perdi de vista porque es stack!
                        strcpy(ptrStr, buffer);
                        break;
                	case 'c':
                		//esto parsea un char
                		parseNextString(inputStr, buffer, format[formatIdx+1], inputStrIdx);
                		char* ptrChar = va_arg(l, char*);
                		*ptrChar = inputStr[inputStrIdx];
                        break;
                	case 'b':
                		//esto parsea una cadena("true" o "false") y la guarda en un puntero pasado por parametro
                		//sigo iterando hasta encontrar un espacio , otro parametro o el fin de cadena
                		inputStrIdx = parseNextString(inputStr, buffer, format[formatIdx+1], inputStrIdx);
                		bool* boolPtr = va_arg(l, bool*);
                		if(strcmp(buffer, "true") == 0){
                			*boolPtr = true;
                		}else{
							*boolPtr = false;
                		}
                        break;
				}

			}//else fin de cadena

		}//else no nos importa este caracter

		continueParsing = inputStr[formatIdx] != '\0';
		formatIdx++;
	}
}

//-------------- Start Console FORMATTED reading functions -------------------