#include "utils.h"
#include "i386.h"
#include "screen_utils.h"
#include "contextManager.h"
#include "screen_estado.h"
#include "defines.h"
#include "colors.h"
#include "mmu.h"

//NOTA:ojo que como es puntero a short, incrementar el puntero corre solo sizeof(unsigned short int) = 2 bytes
unsigned short int* videoBufferEstadoPtr = (unsigned short int*) 0x2D000;

char* nombreDelGrupo = "Crema Americana/Persicco";

//char bandera[BANDERAS_ALTO*BANDERAS_ANCHO*2/*dos chars por px: formato + char*/] = 
//{ 	C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_MAGENTA, ' '  , C_BG_MAGENTA, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ' ,
//	C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_MAGENTA, ' '  , C_BG_MAGENTA, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ' ,
//	C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' '  , C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ',   C_BG_MAGENTA, ' ' ,
//	C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_MAGENTA, ' '  , C_BG_MAGENTA, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ' ,
//	C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_MAGENTA, ' '  , C_BG_MAGENTA, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ',   C_BG_BLUE, ' ' 
//};

char statusBarFormat[VIDEO_COLS] = {
	blackOnWhite,//white border
	whiteOnBlack, whiteOnBlack, whiteOnBlack,//black padding
	whiteOnRed, whiteOnRed, whiteOnBlack,//red task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack,//white task
	blackOnWhite, blackOnWhite, whiteOnBlack, whiteOnBlack, whiteOnBlack,//black padding
	whiteOnRed, whiteOnRed, whiteOnBlack,//red task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
	blackOnOrange, blackOnOrange, whiteOnBlack,//white task
};


void pintarPantallaEstado(){
	
	clearBuffer(videoBufferEstadoPtr);

	//escribir titulo de la ventana
	char* titleStr = getEstadoTitle();
	writeBytesToBuffer(videoBufferEstadoPtr/*+1correr al 2do caracter*/, titleStr, _modoEscrituraTexto, 0/*offset*/, strlen(titleStr));

	dibujarBanderas();
	dibujarConsolaRegistros(getEDI(), getESI(), getEBP(), getESP(), getEBX(), getEDX(), getECX(), getEAX(), getEFLAGS(), getEIP());//actualiza todos los valores de los registros
	dibujarLineaUltimoError(-1/*ningun error todavia*/, 0);
	dibujarConsolaTareas();

}

/**
	Dibuja un buffer de 5*10 a partir de la posicion indicada por startPtr con los modos de escritura descriptos en banderaColors
*/
void drawBandera(char* bandera, char* nombreBandera, unsigned int startPos){
	unsigned int filIdx = 0;	
	//nombre del navio
	writeBytesToBuffer(videoBufferEstadoPtr, nombreBandera, _modoEscrituraFillWhite, startPos+2, strlen(nombreBandera));
	startPos+=VIDEO_COLS;
	while(filIdx<BANDERAS_ALTO){

		unsigned int idxCol = 0;//indice de la linea
		while(idxCol<BANDERAS_ANCHO)
		{
			unsigned char _color = *(bandera + filIdx*2*10 + 2*idxCol + 1);
			unsigned char _caracter = *(bandera + filIdx*2*10 + 2*idxCol);
			unsigned short int pixel = (_color << 8) | _caracter;
			*(videoBufferEstadoPtr + startPos + idxCol) = pixel;
			idxCol++;//avanza de 2 bytes pq es ptr a short
		}

		filIdx++;
		//incremento altura en buffer una fila hacia abajo
		startPos+=VIDEO_COLS;
	}
}

char* getEstadoTitle(){
	return nombreDelGrupo;
}


//-----------------------------------------------------
void dibujarBandera(unsigned int flagNumber, char* bandera){
	void* banderaFisica = (void*) (mmu_virtual2physic((unsigned int)bandera, mmu_get_task_pageDirAddress(flagNumber)));
	unsigned int startPos = 2/*padding entre banderas*/ + 2*VIDEO_COLS/*a partir de la segunda fila*/;
	char* banderaIdStr = "NAVIO x";
	banderaIdStr[9]=/*ASCII numbers start*/ 48 + flagNumber + 1/*indice empieza en 0*/;
	if(flagNumber<4){
		startPos += flagNumber * (2/*padding entre banderas*/ + BANDERAS_ANCHO);
		drawBandera(banderaFisica, banderaIdStr, startPos);
	}else{
		startPos = 34/*padding entre banderas*/ + 8*VIDEO_COLS/*a partir de la octava fila*/;
		startPos += flagNumber * (2/*padding entre banderas*/ + BANDERAS_ANCHO);
		drawBandera(banderaFisica, banderaIdStr,  startPos);
	}
}

void dibujarBanderas(){

	//frame de banderas
	Rectangulo frameBanderas;
	frameBanderas.color = blackOnWhite;
	frameBanderas.ancho = FRAME_BANDERAS_ANCHO;
	frameBanderas.alto = FRAME_BANDERAS_ALTO;
	frameBanderas.x = 0;
	frameBanderas.y = 1;

	dibujarRectangulo(&frameBanderas);

	//incrustar banderas en el frame	
	//unsigned int flagNumber=0;	

	/*for(flagNumber=0;flagNumber<CANT_TAREAS;flagNumber++){
		dibujarBandera(flagNumber, getBandera(flagNumber));
	}*/
}

void dibujarConsolaRegistros(unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EFLAGS, unsigned int EIP){


	//-----------------Fondo negro----------------------//

	Rectangulo frameConsola;
	frameConsola.color = blackOnOrange;
	frameConsola.ancho = FRAME_CONSOLA_ANCHO;
	frameConsola.alto = FRAME_CONSOLA_ALTO;
	frameConsola.x = FRAME_BANDERAS_ANCHO; //a continuacion del mismo
	frameConsola.y = 2; //1 esta el mensajito de error

	dibujarRectangulo(&frameConsola);

	//-----------------Renglon gris inferior----------------------//


	frameConsola.color = blackOnWhite;
	frameConsola.ancho = FRAME_CONSOLA_ANCHO;
	frameConsola.alto = 1;
	frameConsola.x = FRAME_BANDERAS_ANCHO; //a continuacion del mismo
	frameConsola.y = FRAME_CONSOLA_ALTO+1; //1 esta el mensajito de error

	dibujarRectangulo(&frameConsola);
	
	//escribo la primer columna de registros
	
	//basicamente aca, fijate en utils.h que te deje la cucucha del maestruli para hacer get<Registro> y que te de un unsigned int(los 32 bits del registro)
	//en utils.h agregue itoa(integer to ascii), strrev(reverse string), y decToHexStr(integer a string en hexa) para habilitar la locura moderna de estos dias....

	notificarRegistro( "EAX", EAX/*getEAX()*/, 0);
	notificarRegistro( "EBX", EBX/*getEBX()*/, 1);
	notificarRegistro( "ECX", ECX/*getECX()*/, 2);
	notificarRegistro( "EDX", EDX/*getEDX()*/, 3);
	notificarRegistro( "ESI", ESI/*getESI()*/, 4);
	notificarRegistro( "EDI", EDI/*getEDI()*/, 5);
	notificarRegistro( "EBP", EBP/*getEBP()*/, 6);
	notificarRegistro( "ESP", ESP/*getESP()*/, 7);
	notificarRegistro( "EIP", EIP/*getEIP()*/, 8);
	notificarRegistro( "CR0", getCR0(), 9);
	notificarRegistro( "CR2", getCR2(), 10);
	notificarRegistro( "CR3", getCR3(), 11);
	notificarRegistro( "CR4", getCR4(), 12);
//segunda columna
	notificarRegistro( "CS", getCS(), 13);
	notificarRegistro( "DS", getDS(), 14);
	notificarRegistro( "ES", getES(), 15);
	notificarRegistro( "FS", getFS(), 16);
	notificarRegistro( "GS", getGS(), 17);
	notificarRegistro( "SS", getSS(), 18);
	notificarRegistro( "EFLAGS", EFLAGS/*getEFLAGS()*/, 20);


	frameConsola.color = blackOnWhite;
	frameConsola.ancho = 1;
	frameConsola.alto = FRAME_CONSOLA_ALTO + 1;
	frameConsola.x = FRAME_CONSOLA_ANCHO+FRAME_BANDERAS_ANCHO; 
	frameConsola.y = 1; 

	dibujarRectangulo(&frameConsola);
	
}

void notificarRegistro(char* registro, unsigned int valor, int indice){
	indice++; // no importa es por el espacio del inicio
	int padding = 50;
	char buffer[50];

	if(indice>REGS_PER_COLUMN){//nueva columna
		indice-=REGS_PER_COLUMN;
		padding+= FRAME_CONSOLA_ANCHO/2 + 1;
	}

	int offset = (indice)*VIDEO_COLS + padding + VIDEO_COLS ;//(no contamos el status de excepciones)

	if (strlen(registro)>4){

		writeBytesToBuffer(videoBufferEstadoPtr, registro, blackOnOrange, offset + 1 /*margin*/, strlen(registro));	
		decToHexStr(getEFLAGS(), buffer, "", 0);
		
		indice++; //renglon de abajo
		offset = (indice)*VIDEO_COLS + padding + VIDEO_COLS ;//(no contamos el status de excepciones)

		writeBytesToBuffer(videoBufferEstadoPtr, &(buffer[0]), blackOnOrange, offset + 3 /*margin*/, strlen(&(buffer[0])));

	}else{

		decToHexStr(valor, buffer, registro, 0);
		writeBytesToBuffer(videoBufferEstadoPtr, &(buffer[0]), blackOnOrange, offset + 1 /*margin*/, strlen(&(buffer[0])));
	}

}

void dibujarLineaUltimoError(int codError, unsigned int taskNumber){

	//clear line
	char buffer[29/*numero de caracteres a completar en ese bloque de pantalla*/];
	fillString(buffer, 176/*' '*/, 28);
	writeBytesToBuffer(videoBufferEstadoPtr, &(buffer[0]), blackOnWhite, FRAME_BANDERAS_ANCHO + 80, 29/*numero de caracteres a completar en ese bloque de pantalla*/);
	if(codError>=0){
		//------------Fondo cian--------------------//
		//-----------------Fondo negro----------------------//

		Rectangulo frame;
		frame.color = whiteOnRed;
		frame.ancho = FRAME_CONSOLA_ANCHO;
		frame.alto = 1;
		frame.x = FRAME_BANDERAS_ANCHO; //a continuacion del mismo
		frame.y = 1; //1 esta el mensajito de error

		char * navioStr = "Navio x";
		navioStr[6] = (unsigned char) (49 + taskNumber);//itoa cavernicola para un digito

		dibujarRectangulo(&frame);

		char* str = getError(codError);
		/* corto el string sino se pasa a la sig linea algunos err descriptions strlen(string)*/
		writeBytesToBuffer(videoBufferEstadoPtr, str, whiteOnRed, FRAME_BANDERAS_ANCHO + VIDEO_COLS, MIN(29, strlen(str)));
//		printColoredString(videoBufferEstadoPtr, getError(codError), whiteOnRed, FRAME_BANDERAS_ANCHO, 1);

		printColoredString(videoBufferEstadoPtr, navioStr, whiteOnRed, FRAME_BANDERAS_ANCHO + 22, 1);

	}	
}

void dibujarConsolaTareas(){
	actualizarLineaTarea(0, -1/*no error*/);
	actualizarLineaTarea(1, -1/*no error*/);
	actualizarLineaTarea(2, -1/*no error*/);
	actualizarLineaTarea(3, -1/*no error*/);
	actualizarLineaTarea(4, -1/*no error*/);
	actualizarLineaTarea(5, -1/*no error*/);
	actualizarLineaTarea(6, -1/*no error*/);
	actualizarLineaTarea(7, -1/*no error*/);
}

void actualizarLineaTarea(int tarea, int codError){
	//con esta funcion obtenemos
	//unsigned int mmu_virtual2physic(unsigned int virtual, pagedir_entry* pageDirBase);

	char numero[3];
	//itoa(tarea + 1, numero);	
	numero[0] = '#';
	numero[1] = 49 + tarea;
	numero[2] = '\0';
	unsigned short int color = blackOnCyan;
	int posicionLinea_y = FRAME_BANDERAS_ALTO + tarea + 1;
	char buffer[50];//buffer para procesar strings

	if(codError>=0){//-1 es sin error
		color = whiteOnRed;
	}

//----------------Fondo de color adecuado-----------------//
	Rectangulo frame;
	frame.color = color;
	frame.ancho = VIDEO_COLS - 2;
	frame.alto = 1;
	frame.x = 1; 
	frame.y = posicionLinea_y;// a continuacion de las banderas

	dibujarRectangulo(&frame);
	int padding=0;
//numero de tarea
	printColoredString(videoBufferEstadoPtr, numero, blackOnOrange, padding, posicionLinea_y);
//primera pagina
	padding=2;
	printColoredString(videoBufferEstadoPtr, " P1:", color, padding, posicionLinea_y);
	padding=6;
	//decToHexStr(mmu_virtual2physic(TASK_VIRTUAL_P1/*0x40000000*/, mmu_get_task_pageDirAddress(tarea)), buffer, "", 1);
	decToHexStr(getMar1Fisico(tarea), buffer, "", 1);
	printColoredString(videoBufferEstadoPtr, buffer, color, padding, posicionLinea_y);
//segunda pagina
	padding=17;
	printColoredString(videoBufferEstadoPtr, " P2:", color, padding, posicionLinea_y);
	padding=21;
	decToHexStr(getMar2Fisico(tarea), buffer, "", 1);
	printColoredString(videoBufferEstadoPtr, buffer, color, padding, posicionLinea_y);
//tercer pagina
	padding=32;
	printColoredString(videoBufferEstadoPtr, " P3:", color, padding, posicionLinea_y);
	padding=36;
	decToHexStr(getAnclaFisica(tarea), buffer, "", 1);
	printColoredString(videoBufferEstadoPtr, buffer, color, padding, posicionLinea_y);
//label error
	if(codError>=0){
		char* str = getError(codError);
		/* corto el string sino se pasa a la sig linea algunos err descriptions strlen(string)*/
		writeBytesToBuffer(videoBufferEstadoPtr, str, color, VIDEO_COLS - 30 + posicionLinea_y*VIDEO_COLS, MIN(29, strlen(str))/* corto el string sino se pasa a la sig linea algunos err descriptions strlen(string)*/);
//		printColoredString(videoBufferEstadoPtr, str, color, VIDEO_COLS - 30, posicionLinea_y);
	}

	//gris al final de la linea
	char* aux = " ";
	aux[0]=176;
	printColoredString(videoBufferEstadoPtr, aux, blackOnWhite, VIDEO_COLS - 1, posicionLinea_y);

}
