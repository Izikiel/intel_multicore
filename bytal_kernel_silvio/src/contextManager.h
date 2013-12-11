#include "defines.h"

#ifndef __CONTEXT_MANAGER_H__
#define __CONTEXT_MANAGER_H__

typedef struct str_tarea{
     unsigned int p1;
     unsigned int p2;
     unsigned int p3;
     int excep;//tiene que ser signado pues -1 es no excep!
     unsigned short int numero;
     int taskClock;// de 0 a 3, -1 para reloj muerto...como pipo
     int flagClock;
     unsigned int gdtTaskIndex;
     unsigned int gdtFlagIndex;
} __attribute__((__packed__)) datos_tarea;

//Funciones para la isr y mmu
void notificarCambioPagina(unsigned int numeroPaginaTarea, unsigned int fisica, unsigned int taskNumber);
void notificarExcepcion(int errorCode, unsigned int taskNumber, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
void notificarTeclaNumerica(unsigned char number, unsigned char format);
void notificarCanonazo(unsigned int memoryPosDst);
void notificarCambioPantalla(int screenIdSelected);
void notificarRelojTick();
unsigned int getIdleClock();
void notificarRelojIdle();


void setAncla(unsigned int address, int tarea);
void setMar1(unsigned int address, int tarea);
void setMar2(unsigned int address, int tarea);
void setExcepcion(int excepcion, int tarea);
void killClock(unsigned int tarea);
void notificarFinJuego(unsigned int winnerTask);
void haltCpu();
void setEstadoTarea(unsigned int estado);
unsigned int isEjecutandoBandera();
void setEjecutandoBandera(unsigned int value);

//Funciones para la pantalla ESTADO
unsigned int getAnclaFisica(unsigned int nro_tarea);
unsigned int getMar1Fisico(unsigned int nro_tarea);
unsigned int getMar2Fisico(unsigned int nro_tarea);
/*es signado!*/int getExceptionCode(unsigned int nro_tarea);


//Funciones para la pantalla MAPA
unsigned int getAnclaMapa(unsigned int nro_tarea);
unsigned int getMar1Mapa(unsigned int nro_tarea);
unsigned int getMar2Mapa(unsigned int nro_tarea);
//Devuelve la cantidad de paginas totales que apuntan a la posicion del mapa
unsigned int cantPaginasTareasPosicion(unsigned int mapaPos);
//Devuelve el numero de tarea distinto al 'tareaEvaluada' que haya en la posicion, de no haber ninguno
//distinto, retorna -1
unsigned int tareaPosicion(unsigned int mapaPos, unsigned int tareaEvaluada);
void setLastMisilPos(int lastMisilMapPosParam);
int getLastMisilMapPos();
void setLastMisilContent(unsigned short int lastMisilContentParam);
unsigned short int getLastMisilContent();


//Funciones para la barra de relojes
void setGeneralClockNumber(int generalClockNumberParam);
int getGeneralClockNumber();
int getClock(unsigned int tarea);
int getFlagClock(unsigned int tarea);
int getSelectedScreen();

//sched
void inicializar_sched();
void desalojarTarea(unsigned int taskNumber);
unsigned int nextTaskToExecute();
unsigned int isEstadoTarea();//1 es task; 0 es bandera
unsigned int getTareaActual();
void jmpToTask(unsigned short int selector);
//atributos del contextManager implementados en el .c
//comento lo que no quiero hacer visible
//extern unsigned int tarea_actual;
//extern unsigned int bandera_actual;
//extern unsigned int es_tarea;
//extern int lastMisilMapPos;
//extern unsigned short int lastMisilContent;
//extern int selectedScreen;
//extern int generalClockNumber;
//extern char* clockChars;
extern datos_tarea base_tareas[];

#endif  /* !__CONTEXT_MANAGER_H__*/

