#include <screen.h>
#include <defines.h>
#include <utils.h>

//NOTA:ojo que como es puntero a short, incrementar el puntero corre solo sizeof(uint16_t) = 2 bytes
static uint16_t* _outputMemoryPtr = (uint16_t*) VIDEO_MEMORY;

//Indicador de ultima linea impresa, se usa como flag para shiftear
static uint32_t currentLine = 0;
static uint32_t currentCol = 0;

//Indicador de posicion del cursor
static uint32_t cursorLine = 0;
static uint32_t cursorCol = VIDEO_COLS-1;

//variables para impresion de cursor
static char* cursorBuffer = "|/-\\";
static uint32_t cursorIndex = 0;
static uint32_t cursorBufferSize = 4;

void setInitialPrintingLine(uint32_t number){
	currentLine=number;
}

void shiftUpScreen(){
	//backup del buffer de pantalla actual
	uint16_t tmpBuffer[VIDEO_FILS][VIDEO_COLS];
    //copio el buffer de video al buffer temporal
    memcpy(tmpBuffer, _outputMemoryPtr, sizeof(tmpBuffer));
    //copio el buffer a pantalla a partir de la segunda linea
    memcpy(_outputMemoryPtr, tmpBuffer[1], VIDEO_COLS*(VIDEO_FILS-1)*sizeof(uint16_t));        
    //limpio ultima linea
    memset(_outputMemoryPtr + (VIDEO_FILS-1)*VIDEO_COLS, 0, VIDEO_COLS*sizeof(uint16_t));    
}

void clockCursor(){
	//imprime el cursor en la ultima linea de pantalla
	putChar(cursorBuffer[cursorIndex], redOnBlack, VIDEO_COLS-1, 0);
	cursorIndex = (cursorIndex+1) % cursorBufferSize;
}

void printLine(char* cadena, uint8_t format){
	if(currentLine<VIDEO_FILS-1)
	{
		//si estoy dentro de la primer pantalla sin shiftear imprimo normalmente
		printString(cadena, format, 0, currentLine);
		currentLine++;
	}
	else
	{
		//si me excedi de las lineas, shifteo la pantalla una linea hacia arriba
		shiftUpScreen();
		//escribo en la anteultima linea de la pantalla
		printString(cadena, format, 0, VIDEO_FILS-2);//notar que los indices x,y comienzan en cero por eso se le resta uno a VIDEO_FILS
	}
	currentCol=0;

	//pongo la posicion del proximo char a ser escrito en pantalla
	printProxCursor();
}

void printLineNumber(uint32_t number, uint8_t format){
	if(currentLine<VIDEO_FILS-1)
	{
		//si estoy dentro de la primer pantalla sin shiftear imprimo normalmente
		printInteger(number, format, 0, currentLine);
		currentLine++;
	}
	else
	{
		//si me excedi de las lineas, shifteo la pantalla una linea hacia arriba
		shiftUpScreen();
		//escribo en la anteultima linea de la pantalla
		printInteger(number, format, 0, VIDEO_FILS-2);//notar que los indices x,y comienzan en cero por eso se le resta uno a VIDEO_FILSut}
	}
	currentCol=0;

	//pongo la posicion del proximo char a ser escrito en pantalla
	printProxCursor();
}

void putChar(char caracter, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint16_t offset = posX + posY * VIDEO_COLS;	
	uint16_t pixel = (format << 8) | caracter;
	*(_outputMemoryPtr + offset) = pixel;
}

void printString(char* cadena, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint32_t strlength = strlen(cadena);
	uint8_t idx = 0;
	while(idx < strlength)
	{
	putChar(cadena[idx], format, posX + idx, posY);
		idx++;
	}
}

void printInteger(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	itoa(number, buffer);
	printString(buffer, format, posX, posY);
}

static void clearBuffer(unsigned short int* outputBufferPtr){
	uint8_t _modoEscritura = modoEscrituraTexto;//fondo negro, letras blancas
	uint8_t _caracter = ' ';//espacio en blanco

	uint32_t offset = 0;
	while(offset<VIDEO_FILS*VIDEO_COLS)
	{
		uint16_t pixel = (_modoEscritura << 8) | _caracter;
		*(outputBufferPtr + offset) = pixel;
		offset++;
	}
}

void clrscr(){
	clearBuffer(_outputMemoryPtr);
	currentLine=0;
	currentCol=0;
}

void backspace(){
	if(currentCol>3){//espacio para el prefijo "$> "
		currentCol--;
	}
	//por ahora no permito borrar lineas de mas arriba de la actual!
	/*else{
		currentCol=VIDEO_COLS-1;
		currentLine--;
	}*/
	putChar(' ', modoEscrituraTexto, currentCol, currentLine);

	//guardo la posicion del cursor actual
	uint32_t cline = cursorLine;
	uint32_t ccol = cursorCol;

	//pongo la posicion del proximo cursor a ser escrito en pantalla
	cursorLine=currentLine;
	cursorCol=currentCol;

	//limpio cursor
	putChar(' ', modoEscrituraTexto, ccol, cline);
	//pongo cursor nuevo
	putChar(' ', whiteOnRed, currentCol, currentLine);	
}

void printChar(char caracter, uint8_t format){
	if(currentCol == VIDEO_COLS){
		//Linea nueva		
        printLine("", modoEscrituraTexto);
        currentCol=0;
	}
	putChar(caracter, format, currentCol, currentLine);
	currentCol++;

	//pongo la posicion del proximo char a ser escrito en pantalla
	printProxCursor();
}

void getLastScreenLine(char* buffer)//Nota, el buffer devuelto es de tamanio VIDEO_COLS
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

void printProxCursor(){
	cursorLine=currentLine;
	cursorCol=currentCol;
	putChar(' ', whiteOnRed, currentCol, currentLine);
}

void hideCursor(){
	putChar(' ', modoEscrituraTexto, currentCol, currentLine);
}