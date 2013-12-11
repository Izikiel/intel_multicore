#include "contextManager.h"
#include "screen_utils.h"
#include "screen_estado.h"
#include "screen_mapa.h"
#include "mmu.h"
#include "tss.h"
#include "i386.h"
#include "utils.h"
#include "colors.h"
//							PIRATES!!, GO AWAY!!!!!!!
//
//							 .xm*f""??T?@hc.
//                          z@"` '~((!!!!!!!?*m.
//                        z$$$K   ~~(/!!!!!!!!!Mh
//                      .f` "#$k'`~~\!!!!!!!!!!!MMc
//                     :"     f*! ~:~(!!!!!!!!!!XHMk
//                     f      " %n:~(!!!!!!!!!!!HMMM.
//                    d          X~!~(!!!!!!!X!X!SMMR
//                    M :   x::  :~~!>!!!!!!MNWXMMM@R
// n                  E ' *  ueeeeiu(!!XUWWWWWXMRHMMM>                :.
// E%                 E  8 .$$$$$$$$K!!$$$$$$$$&M$RMM>               :"5
//z  %                3  $ 4$$$$$$$$!~!*$$$$$$$$!$MM$               :" `
//K   ":              ?> # '#$$$$$#~!!!!TR$$$$$R?@MME              z   R
//?     %.             5     ^"""~~~:XW!!!!T?T!XSMMM~            :^    J
// ".    ^s             ?.       ~~d$X$NX!!!!!!M!MM             f     :~
//  '+.    #L            *c:.    .~"?!??!!!!!XX@M@~           z"    .*
//    '+     %L           #c`"!+~~~!/!!!!!!@*TM8M           z"    .~
//      ":    '%.         'C*X  .!~!~!!!!!X!!!@RF         .#     +
//        ":    ^%.        9-MX!X!!X~H!!M!N!X$MM        .#`    +"
//          #:    "n       'L'!~M~)H!M!XX!$!XMXF      .+`   .z"
//            #:    ":      R *H$@@$H$*@$@$@$%M~     z`    +"
//              %:   `*L    'k' M!~M~X!!$!@H!tF    z"    z"
//                *:   ^*L   "k ~~~!~!!!!!M!X*   z*   .+"
//                  "s   ^*L  '%:.~~~:!!!!XH"  z#   .*"
//                    #s   ^%L  ^"#4@UU@##"  z#   .*"
//                      #s   ^%L           z#   .r"
//                        #s   ^%.       u#   .r"
//                          #i   '%.   u#   .@"
//                            #s   ^%u#   .@"
//                              #s x#   .*"
//                               x#`  .@%.
//                             x#`  .d"  "%.
//                           xf~  .r" #s   "%.
//                     u   x*`  .r"     #s   "%.  x.
//                     %Mu*`  x*"         #m.  "%zX"
//                     :R(h x*              "h..*dN.
//                   u@NM5e#>                 7?dMRMh.
//                 z$@M@$#"#"                 *""*@MM$hL
//               u@@MM8*                          "*$M@Mh.
//             z$RRM8F"                             "N8@M$bL
//            5`RM$#                                  'R88f)R
//            'h.$"                                     #$x*
//
//
//

datos_tarea base_tareas[CANT_TAREAS] = {
											[0] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_1a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_1b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[1] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_2a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_2b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[2] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_3a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_3b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[3] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_4a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_4b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[4] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_5a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_5b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[5] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_6a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_6b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[6] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_7a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_7b_DESC << 3 /*gdtFlagIndex con rpl 0*/},
											[7] = (datos_tarea) {0x0/*p1*/, 0x0/*p2*/, 0x0/*p3*/, -1/*excep*/, 0x0/*numero*/, 0/*task clock state*/, 0/*flag clock state*/, GDT_IDX_TASK_8a_DESC << 3/*rpl 0*/, GDT_IDX_TASK_8b_DESC << 3 /*gdtFlagIndex con rpl 0*/}
										};
										
//inicializo en -1 porque no se disparo nada todavia										
int lastMisilMapPos = -1;
unsigned short int lastMisilContent = 0;

//inicializo en -1 porque no se selecciono pantalla todavia
int selectedScreen = -1;

//inicializo en -1 porque no se notifico todavia
int generalClockNumber = -1;

unsigned int aliveTasks = 0;//nextTaskToExecute lo actualiza en cada clock
unsigned int ejecutandoBandera=0;
unsigned int banderasEjecutadas = 0;
unsigned int idleClock = 0;
unsigned int contadorTurnoTareaBandera = 0;
unsigned int tarea_actual = 0;// del 0 al 7
unsigned int bandera_actual = 0;
unsigned int es_tarea = 1; //1 es task; 0 es bandera

unsigned int isEjecutandoBandera(){
	return ejecutandoBandera;
}

void setEjecutandoBandera(unsigned int value){
	ejecutandoBandera = value;
}

unsigned int getIdleClock(){
	return idleClock;
}

void notificarRelojIdle(){
	idleClock = (idleClock + 1) % 4;
	refreshIdleClock();
}

void notificarFinJuego(unsigned int winnerTask){
	char* string = "Todas muertas las tareas. Gano # ";
	string[strlen(string)-1] = winnerTask + 49;//ascii del 1 mas tarea.
	printColoredString((unsigned short int*) 0xB8000/*buffer video*/, &(string[0]), whiteOnRed, 28, 11);
	int i=0;
	for(i=0;i<0xFFFF; i++);//hago tiempo para que repinte la pantalla. nasty hack
	haltCpu();
}

void setGeneralClockNumber(int generalClockNumberParam){
	generalClockNumber=generalClockNumberParam;
}

int getGeneralClockNumber(){
	return generalClockNumber;
}

void setSelectedScreen(int selectedScreenParam){
	selectedScreen=selectedScreenParam;
}

int getSelectedScreen(){
	return selectedScreen;
}

void setLastMisilContent(unsigned short int lastMisilContentParam){
	lastMisilContent = lastMisilContentParam;
}

unsigned short int getLastMisilContent(){
	return lastMisilContent;
}

void setLastMisilPos(int lastMisilMapPosParam){
	lastMisilMapPos=lastMisilMapPosParam;
}

int getLastMisilMapPos(){
	return convertMemory2MapPos(lastMisilMapPos);
}

void killClock(unsigned int tarea){
	base_tareas[tarea].taskClock=-1;
	base_tareas[tarea].flagClock=-1;
}


/*
*Pre: Ya se encuentran las tareas inicializadas
*Post: Direccion fisica del ancla de la tarea parametrizada
*/
unsigned int getAnclaFisica(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return datos.p1;
}

unsigned int getMar1Fisico(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return datos.p2;
}

unsigned int getMar2Fisico(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return datos.p3;
}

int getExceptionCode(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return datos.excep;	
}

unsigned int getAnclaMapa(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return convertMemory2MapPos(datos.p1);
}
unsigned int getMar1Mapa(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return convertMemory2MapPos(datos.p2);
}
unsigned int getMar2Mapa(unsigned int nro_tarea){
	datos_tarea datos = base_tareas[nro_tarea];
	return convertMemory2MapPos(datos.p3);
}

int getClock(unsigned int nro_tarea){
	return base_tareas[nro_tarea].taskClock;

}

int getFlagClock(unsigned int nro_tarea){
	return base_tareas[nro_tarea].flagClock;

}

void notificarTeclaNumerica(unsigned char number, unsigned char format)
{	
	writeBytesToBuffer((unsigned short int*) 0x2F000/*buffer mapa*/, (char*)&number, format,  79, 1);
	writeBytesToBuffer((unsigned short int*) 0x2D000/*buffer estado*/, (char*)&number, format,  79, 1);
	
	//llamo a esto para que se repinte la pantalla parcialmente!	
	flushPartialScreenBuffer(getSelectedScreen(), 79, 0, 1);
}

void notificarCanonazo(unsigned int memoryPosDst)
{
	//Reestablezco lo que habia en el anterior misilazo si es que no paso ninguna tarea por encima (si paso por encima entonces ya lo borro)
	sacarMisil();
	setLastMisilPos(memoryPosDst);
	dibujarMisil();
	//repintar buffer correspondiente en pantalla
	//notificarCambioPantalla(getSelectedScreen());
}

void notificarCambioPantalla(int screenIdSelected)
{
	setSelectedScreen(screenIdSelected);
	switch(screenIdSelected){
		case 0:
			flushMapaBufferToScreen();
			break;
		case 1:
			flushEstadoBufferToScreen();
			break;
	}	
}

void notificarRelojTick()
{
	//refresco clock general
	generalClockNumber = (generalClockNumber + 1) % 4;
	refreshGeneralClock();
	
//	refresco contexto, cada 3 clocks paso a ejecutar todas las banderas y despues vuelvo re piola a modo tarea
	if(isEstadoTarea() == 1){
		//refresco clock
		base_tareas[tarea_actual].taskClock = (base_tareas[tarea_actual].taskClock + 1) % 4;
		refreshClock(tarea_actual);

		contadorTurnoTareaBandera++;
		if(contadorTurnoTareaBandera==3){//cada 3 ciclos de tarea cambia a ejecutar todas las banderas
			//it's flags time!
			//reseteo el contador de turnos
			contadorTurnoTareaBandera=0;
			//paso a estado tarea
			setEstadoTarea(0/*estado flag*/);
			//el proximo reloj va a ser de flags
			//breakpoint();
		}
	}else{
		//refresco clocks 
		base_tareas[tarea_actual].flagClock = (base_tareas[tarea_actual].flagClock + 1) % 4;
		refreshFlagClock(tarea_actual);

		banderasEjecutadas++;
		//if(banderasEjecutadas == 4){//cambio de contexto a tarea despues de 4 ejecuciones
		if(banderasEjecutadas>=aliveTasks){//cuando haya ejecutado todas las banderas disponibles, pongo menor porque pueden haber muerto en el medio del contexto de bandera tareas por sus banderas
			//vuelvo modo tarea
			banderasEjecutadas=0;
			setEstadoTarea(1/*estado flag*/);
			//el proximo reloj va a ser de tareas!
			//breakpoint();
		}
	}
	
}

void notificarCambioPagina(unsigned int numeroPaginaTarea, unsigned int fisica, unsigned int taskNumber){
		//las declaro aca y no dentro de cada caso porque sino me tira:
		//"a label can only be part of a statement and a declaration is not a statement" :-o
		//DALE PARA ADELANTE, FIESTA!

		unsigned int pos_mapa_1 = 0;
		unsigned int pos_mapa_2 = 0;
		
	switch(numeroPaginaTarea){
		case 1:

			pos_mapa_1 = convertMemory2MapPos(fisica);
			pos_mapa_2 = convertMemory2MapPos(getMar2Fisico(taskNumber));

			navegarTarea(pos_mapa_1,  pos_mapa_2, taskNumber);
			setMar1(fisica, taskNumber);
			break;

		case 2:

			pos_mapa_1 = convertMemory2MapPos(getMar1Fisico(taskNumber));
			pos_mapa_2 = convertMemory2MapPos(fisica);

			navegarTarea(pos_mapa_1,  pos_mapa_2, taskNumber);
			setMar2(fisica, taskNumber);
			break;

		case 3:

			pos_mapa_1 = convertMemory2MapPos(fisica);
		    remapearAnclaTarea(pos_mapa_1, taskNumber);
			setAncla(fisica, taskNumber);
			break;

	}
	//notificar en la pantalla que cambio la tarea taskNumber en la pagina numeroPaginaTarea
	actualizarLineaTarea(taskNumber, getExceptionCode(taskNumber)/*error code*/);
	//repintar buffer correspondiente en pantalla
	notificarCambioPantalla(getSelectedScreen());
}

void notificarExcepcion(int errorCode, unsigned int taskNumber, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP){
	setExcepcion(errorCode, taskNumber);
	//notificar en la pantalla que cambio la tarea taskNumber con una excepcion errorCode
	actualizarLineaTarea(taskNumber, getExceptionCode(taskNumber)/*error code*/);
	//actualizar registros y error de ultima excepcion
	dibujarConsolaRegistros(EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX, EFLAGS, EIP);//actualiza todos los valores de los registros en pantalla estado
	dibujarLineaUltimoError(errorCode, taskNumber);//expone la descripcion sobre la consola de registros

	//seteo los dos clocks como muertos
	desalojarTarea(taskNumber);

	//actualizar buffer mapa
	borrarTareaDelMapa(taskNumber);
	
	//repintar buffer correspondiente en pantalla
	notificarCambioPantalla(getSelectedScreen());	
}

void setAncla(unsigned int address, int tarea){
	base_tareas[tarea].p1 = address;
}
void setMar1(unsigned int address, int tarea){
	base_tareas[tarea].p2 = address;
}
void setMar2(unsigned int address, int tarea){
	base_tareas[tarea].p3 = address;
}
void setExcepcion(int excepcion, int tarea){
	base_tareas[tarea].excep = excepcion;
}

void desalojarTarea(unsigned int taskNumber){
	killClock(taskNumber);
	refreshClock(taskNumber);
	refreshFlagClock(taskNumber);
}

void inicializar_sched(){

}

/*avanza a la proxima tarea y devuelve su indice en la gdt*/
unsigned int nextTaskToExecute()
{		
	unsigned int tareaActualAlLlamado = getTareaActual();
	unsigned int contadorVivas=0;
	int index = 0;
	for(index=0;index<CANT_TAREAS;index++){
		contadorVivas += (base_tareas[index].excep < 0/*-1 es sin error, el resto son error codes*/);		
	}

	//actualizo tareas vivas
	aliveTasks = contadorVivas;
	if(aliveTasks == 0){
		notificarFinJuego(tareaActualAlLlamado);
	}
	
	unsigned int vueltas = 0;
	do{
		tarea_actual = (tarea_actual + 1) % 8;
		vueltas++;
	}while( (base_tareas[tarea_actual].excep >= 0/*-1 es sin error, el resto son error codes*/) && (aliveTasks>0)  && (vueltas < 8));

	//debug only!
	//return base_tareas[tarea_actual].gdtTaskIndex;

	if(isEstadoTarea() == 1){
		return base_tareas[tarea_actual].gdtTaskIndex;
	}else{		
		setEjecutandoBandera(1);
		tss_reiniciar_esp_bandera(tarea_actual);
		return base_tareas[tarea_actual].gdtFlagIndex;
	}
}

//1 es task; 0 es bandera
unsigned int isEstadoTarea()
{
	return es_tarea;
}

void setEstadoTarea(unsigned int estado){
	es_tarea = estado;
}

unsigned int cantPaginasTareasPosicion(unsigned int mapaPos){

	unsigned int fisica = convertMap2MemoryPos(mapaPos);
	int i, cant = 0;
	
	for (i=0;i<CANT_TAREAS;i++){
		if (base_tareas[i].p1==fisica){
			cant++;
		}
		if (base_tareas[i].p2==fisica){
			cant++;
		}
		if (base_tareas[i].p3==fisica){
			cant++;
		}
	}

	return cant;
}

unsigned int getTareaActual(){
	return tarea_actual;
}

unsigned int tareaPosicion(unsigned int mapaPos, unsigned int tareaEvaluada){

	unsigned int fisica = convertMap2MemoryPos(mapaPos);
	int i = 0;
	int tarea = -1; //numero de tarea invalido (tarea nula)
	
	for (i=0;i<CANT_TAREAS;i++){
		if ((base_tareas[i].p1==fisica || base_tareas[i].p2==fisica || base_tareas[i].p3==fisica) && tareaEvaluada!= i){
			tarea = i;
			break;
		}
	}
	
	return tarea;
}

