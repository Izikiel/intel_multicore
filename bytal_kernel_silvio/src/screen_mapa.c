#include "utils.h"
#include "contextManager.h"
#include "screen_utils.h"
#include "screen_mapa.h"
#include "defines.h"
#include "colors.h"

#define CANT_POS_TIERRA 256
#define CANT_POS_MAR 1664

unsigned short int* videoBufferMapaPtr = (unsigned short int*) 0x2F000;

char marChar = 176;
char tierraChar = 178;
char compartidoChar = 'X';
char tierraFormato = blackOnOrange;
char markedPosFormato = whiteOnRed;//blackOnGreen;
char marFormato = blackOnCyan;

char formatoMar[CANT_POS_MAR];
char formatoTierra[CANT_POS_TIERRA];
char contenidoMar[CANT_POS_MAR];
char contenidoTierra[CANT_POS_TIERRA];


void inicializarBuffersMapa(){
	//inicializo los buffers
	fillString(&(formatoMar[0]), marFormato, CANT_POS_MAR);
	fillString(&(formatoTierra[0]), tierraFormato, CANT_POS_TIERRA);
	fillString(&(contenidoMar[0]), marChar, CANT_POS_MAR);
	fillString(&(contenidoTierra[0]), tierraChar, CANT_POS_TIERRA);
}

void pintarPantallaMapa(){
	writeFormattedBytesToBuffer(videoBufferMapaPtr, &(contenidoTierra[0]), &(formatoTierra[0]),  0, CANT_POS_TIERRA);
	writeFormattedBytesToBuffer(videoBufferMapaPtr, &(contenidoMar[0]), &(formatoMar[0]),  CANT_POS_TIERRA, CANT_POS_MAR);
}

void pintarPosicionMapa(unsigned int mapPos, char formato, char caracter){
	writeFormattedBytesToBuffer(videoBufferMapaPtr, &(caracter), &(formato),  mapPos, 1/*un solo char*/);
}
/*
Dir memoria son 4k aligned
void pintarPosicionMemoria(unsigned int memoryPos, char formato, char caracter){
	unsigned int mapPos = convertMemory2MapPos(memoryPos);
	writeFormattedBytesToBuffer(videoBufferMapaPtr, &(caracter), &(formato),  mapPos, 1);
}
*/

/*Dir memoria son 4k aligned*/
void remapearAnclaTarea(unsigned int mapPos, unsigned int taskNumber){
	
	unsigned int mapPosToRestore = getAnclaMapa(taskNumber);
	sacarElementoTareaPosicion(mapPosToRestore, taskNumber, tierraFormato, tierraChar);
	//llego a la nueva posicion
	agregarElementoTareaPosicion(mapPos, taskNumber, tierraFormato);
}
/*Dir memoria son 4k aligned*/
void navegarTarea(unsigned int firstMapPos, unsigned int secondMapPos, unsigned int taskNumber){
	
	unsigned int prev1PageLocation = getMar1Mapa(taskNumber);
	unsigned int prev2PageLocation = getMar2Mapa(taskNumber);

	//limpio posiciones anteriores
	sacarElementoTareaPosicion(prev1PageLocation, taskNumber, marFormato, marChar);
	sacarElementoTareaPosicion(prev2PageLocation, taskNumber, marFormato, marChar);

	//marco nuevas posiciones
	agregarElementoTareaPosicion(firstMapPos, taskNumber, marFormato);
	agregarElementoTareaPosicion(secondMapPos, taskNumber, marFormato);
	
}

void sacarElementoTareaPosicion(unsigned int mapPos,unsigned int taskNumber, char formato, unsigned char emptyChar){

	
	if (tareaPosicion(mapPos, taskNumber)==-1 && cantPaginasTareasPosicion(mapPos)==1){
		//si no quedan mas paginas de nadie en el lugar que voy a abandonar
		pintarPosicionMapa(mapPos, formato, emptyChar);
	}

	if (tareaPosicion(mapPos, taskNumber)!=-1 && cantPaginasTareasPosicion(mapPos)>1){
		//si quedan mas de 2 paginas y hay tareas que no es la actual entonces el lugar es compartido
		pintarPosicionMapa(mapPos, markedPosFormato, compartidoChar);
	}

	if (tareaPosicion(mapPos, taskNumber)==-1 && cantPaginasTareasPosicion(mapPos)==2){
		//solo queda una tarea diferente a  la mia
		char tskStr = taskNumber+49;
		pintarPosicionMapa(mapPos, markedPosFormato, tskStr) ;
	}
	if (tareaPosicion(mapPos, taskNumber)!=-1 && cantPaginasTareasPosicion(mapPos)==2){
		//solo queda una tarea diferente a  la mia
		char tskStr = tareaPosicion(mapPos, taskNumber)+49;
		pintarPosicionMapa(mapPos, markedPosFormato, tskStr) ;
	}

}

void agregarElementoTareaPosicion(unsigned int mapPos,unsigned int taskNumber, char formato ){

	char taskNumberStr = 49 + taskNumber;//49 es el ascii del 1

	if (tareaPosicion(mapPos, taskNumber)==-1){
		pintarPosicionMapa(mapPos, markedPosFormato, taskNumberStr);
		
	}else{
		pintarPosicionMapa(mapPos, markedPosFormato, compartidoChar);
	}


}

void borrarTareaDelMapa(unsigned int tarea){
	sacarElementoTareaPosicion(getAnclaMapa(tarea), tarea, tierraFormato, tierraChar);
	sacarElementoTareaPosicion(getMar1Mapa(tarea), tarea, marFormato, marChar);
	sacarElementoTareaPosicion(getMar2Mapa(tarea), tarea, marFormato, marChar);
}

void sacarMisil(){
	int mapPos = getLastMisilMapPos();
	int pagsEnPos = cantPaginasTareasPosicion(mapPos);

	if(pagsEnPos==0){
		//no hay tareas entonces pongo agua
		pintarPosicionMapa(mapPos, marFormato, marChar);
	}else{
	//si hay algo entonces dejo ese algo, no obstante, tengo que ver que algo es lo que hay ahi
		if(pagsEnPos==1){
			char tskStr = tareaPosicion(mapPos, -1) + 49;//le mando una tarea invalida asi me devuelve el nro de tarea que esta ahi
			pintarPosicionMapa(mapPos, markedPosFormato, tskStr);
		}else{
			pintarPosicionMapa(mapPos, markedPosFormato, 'X');
		}
	}

	if(getSelectedScreen()==0){
		flushPartialScreenBufferOffset(0, mapPos, 1);
	}
}

void dibujarMisil(){
	int mapPos = getLastMisilMapPos();
	pintarPosicionMapa(mapPos, misilMark, '*');
	if(getSelectedScreen()==0){
		flushPartialScreenBufferOffset(0, mapPos, 1);
	}

}