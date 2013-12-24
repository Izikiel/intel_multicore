#include <console.h>
#include <defines.h>
#include <utils.h>
#include <vargs.h>

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

//-------------- Start Config functions -------------------

void scrn_setYCursor(uint32_t number){
	currentLine=number;
}

void scrn_setXCursor(uint32_t number){
	currentCol=number;
}

void scrn_update_text_cursor(){
	cursorLine=currentLine;
	cursorCol=currentCol;
	scrn_pos_putc(' ', whiteOnGreen, currentCol, currentLine);
}

void scrn_hide_text_cursor(){
	scrn_pos_putc(' ', modoEscrituraTexto, currentCol, currentLine);
}

void scrn_initialize_console()
{
	scrn_putc('$', redOnBlack);
    scrn_putc('>', redOnBlack);
    scrn_putc(' ', redOnBlack);
}

void scrn_reset_console()
{
	scrn_clear();
	scrn_putc('$', redOnBlack);
    scrn_putc('>', redOnBlack);
    scrn_putc(' ', redOnBlack);
}

//-------------- End Config functions -------------------

//-------------- Start Console movement functions -------------------

void scrn_moveUp(){
	//backup del buffer de pantalla actual
	uint16_t tmpBuffer[VIDEO_FILS][VIDEO_COLS];
    //copio el buffer de video al buffer temporal
    memcpy(tmpBuffer, _outputMemoryPtr, sizeof(tmpBuffer));
    //copio el buffer a pantalla a partir de la segunda linea
    memcpy(_outputMemoryPtr, tmpBuffer[1], VIDEO_COLS*(VIDEO_FILS-1)*sizeof(uint16_t));        
    //limpio ultima linea
    memset(_outputMemoryPtr + (VIDEO_FILS-1)*VIDEO_COLS, 0, VIDEO_COLS*sizeof(uint16_t));    
}

void scrn_moveBack(bool fromSystem){
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
	scrn_pos_putc(' ', modoEscrituraTexto, currentCol, currentLine);

	//guardo la posicion del cursor actual
	uint32_t cline = cursorLine;
	uint32_t ccol = cursorCol;

	//pongo la posicion del proximo cursor a ser escrito en pantalla
	cursorLine=currentLine;
	cursorCol=currentCol;

	//limpio cursor
	scrn_pos_putc(' ', modoEscrituraTexto, ccol, cline);
	//pongo cursor nuevo
	scrn_pos_putc(' ', whiteOnGreen, currentCol, currentLine);	
}

void scrn_clear(){
	uint8_t _modoEscritura = modoEscrituraTexto;//fondo negro, letras blancas
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
void scrn_print_next_cursor(){
	//imprime el cursor en la ultima linea de pantalla
	scrn_pos_putc(cursorBuffer[cursorIndex], whiteOnBlue, VIDEO_COLS-1, 0);
	cursorIndex = (cursorIndex+1) % cursorBufferSize;
}

//-------------- End Console clock cursor functions -------------------

//-------------- Start Console FORMATTED printing functions -------------------

// Tomado de juampi OS
void scrn_printf(const char* msg, ...)
{
        va_list l;
        va_start(l,msg);
        scrn_vprintf(msg, l);
        va_end(l);
}

// Tomado de juampi OS
void scrn_vprintf(const char* msg, va_list l)
{
        uint i;
        char buffer[64];
        for(i = 0; msg[i]; i++) {
                switch(msg[i]) {
                case '%':
                        i++;
                        switch(msg[i]) {
                        case '%':
                                scrn_putc(msg[i], modoEscrituraTexto);
                                break;
                        case 'u':
                        		decToHexStr(va_arg(l, uint), buffer, ""/*no title*/, 1/*print 0x prefix*/);
                                scrn_puts(buffer, modoEscrituraTexto);
                                break;
                        case 'd':
                                itoa(va_arg(l, uint), buffer);                                
                                scrn_puts(buffer, modoEscrituraTexto);
                                break;
                        case 's':
                                scrn_puts(va_arg(l, char*), modoEscrituraTexto);
                                break;
                        case 'c':
                                scrn_putc(va_arg(l, uint), modoEscrituraTexto);
                                break;
                        case 'b':
                        		scrn_puts(va_arg(l, uint) ? "true" : "false", modoEscrituraTexto);
                                break;
                        }
                        break;
                default:
                        scrn_putc(msg[i], modoEscrituraTexto);
                        break;
                }
        }
}

//-------------- End Console FORMATTED printing functions -------------------

//-------------- Start Console printing functions -------------------

void scrn_println(char* cadena, uint8_t format){
	if(currentLine<VIDEO_FILS-1)
	{
		//si estoy dentro de la primer pantalla sin shiftear imprimo normalmente
		scrn_pos_print(cadena, format, 0, currentLine);
		currentLine++;
	}
	else
	{
		//si me excedi de las lineas, shifteo la pantalla una linea hacia arriba
		scrn_moveUp();
		//escribo en la anteultima linea de la pantalla
		scrn_pos_print(cadena, format, 0, VIDEO_FILS-2);//notar que los indices x,y comienzan en cero por eso se le resta uno a VIDEO_FILS
	}
	currentCol=0;

	//pongo la posicion del proximo char a ser escrito en pantalla
	scrn_update_text_cursor();
}

//como no existe la sobrecarga de funciones, me armo otra funcion para imprimir numeros.
void scrn_printlnNumber(uint32_t number, uint8_t format){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	memset(buffer, '\0', VIDEO_FILS*VIDEO_COLS);
	itoa(number, buffer);
	scrn_println(buffer, format);
}

void scrn_putc(char caracter, uint8_t format){
	//process console movement
	if(currentCol == VIDEO_COLS){
		//Linea nueva		
        scrn_println("", modoEscrituraTexto);
        currentCol=0;
	}

	//process \n, \t, etc special chars
	switch(caracter){
		case '\n': //Linea nueva
			//quitar el cursor de la pantalla
            scrn_hide_text_cursor();
	        scrn_println("", modoEscrituraTexto);
			//quitar el cursor de la pantalla
            scrn_hide_text_cursor();
	        currentCol=0;
			break;
		case '\r': //Borrar linea actual
	        while(currentCol>0){
	        	scrn_moveBack(false/*not calling from system*/);;
	        }
			break;
		case '\t': //Tab
            //4 espacios
            scrn_putc(' ', modoEscrituraTexto);
            scrn_putc(' ', modoEscrituraTexto);
            scrn_putc(' ', modoEscrituraTexto);
            scrn_putc(' ', modoEscrituraTexto);  
			//quitar el cursor de la pantalla
            scrn_hide_text_cursor();            
			break;
		default:
			scrn_pos_putc(caracter, format, currentCol, currentLine);
			//avanzar columna
			currentCol++;
			break;
	}

	//pongo la posicion del proximo char a ser escrito en pantalla
	scrn_update_text_cursor();
}

void scrn_puts(char* string, uint8_t format)
{
	uint32_t len = strlen(string);
	uint32_t i = 0;
	while(i<len){
		scrn_putc(string[i], format);
		i++;
	}
}
//-------------- End Console printing functions -------------------

//-------------- Start Console POSITIONED printing functions -------------------

void scrn_pos_putc(char caracter, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint16_t offset = posX + posY * VIDEO_COLS;	
	uint16_t pixel = (format << 8) | caracter;
	*(_outputMemoryPtr + offset) = pixel;
}

void scrn_pos_print(char* cadena, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint32_t strlength = strlen(cadena);
	uint8_t idx = 0;
	while(idx < strlength)
	{
		if(posX + idx > VIDEO_COLS){
			posY++;
			posX=0;
		}
		scrn_pos_putc(cadena[idx], format, posX + idx, posY);
		idx++;
	}
}

void scrn_pos_printInt(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	itoa(number, buffer);
	scrn_pos_print(buffer, format, posX, posY);
}

//-------------- End Console POSITIONED printing functions -------------------

//-------------- Start Console reading functions -------------------

void scrn_get_last_line(char* buffer)//Nota, el buffer devuelto es de tamanio VIDEO_COLS
{
	//voy a leer a partir de la ultima linea escrita la cantidad de caracteres indicada por currentCol, 
	//salteandome los caracteres de formato y salteandome los primeros 3 chars del simbolo del sistema
	uint16_t offset = currentLine*VIDEO_COLS;
	uint16_t* screenPtr = _outputMemoryPtr + offset;
	int i=3;
	while(i<currentCol){
		uint16_t readFormattedChar = *(screenPtr + i);
		char readChar = (uint8_t)readFormattedChar;
		buffer[i-3] = readChar;
		i++;
	}
}

//-------------- End Console reading functions -------------------
