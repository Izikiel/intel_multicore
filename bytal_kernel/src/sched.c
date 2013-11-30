/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del scheduler
*/

#include "sched.h"
#include "i386.h"

#define true 1
#define false 0

int tasks[8];
int index = -1;
int bandera = 0;
int start_flag = -1;
unsigned char bandera_mode = false;

void sched_inicializar() {
	int i;
	for (i = 0; i < CANT_TAREAS; i++)
		tasks[i] = ((25 + i) << 3);
}


unsigned short sched_proximo_indice() {
	if ((index + 1) == CANT_TAREAS)
		index = 0;
	else
		index++;
    return tasks[index];
}

void kill_task(int t_index){
	tasks[t_index] = 0;
}

unsigned char bandera_time(){
	if (get_live_tareas() == 0)
		return false;

	if (bandera_mode)
		return true;

	if (bandera == 3){
		bandera = 0;
		start_flag = index;
		current_flag = index;
		bandera_mode = true;
		return true;
	}
	else{
		bandera++;
		return false;
	}
}

void next_flag(){
	while(1){
		if ((current_flag +1) == CANT_TAREAS)
			current_flag = 0;
		else
			current_flag++;
		if (current_flag == start_flag){
			bandera_mode = false;
			break;
		}
		if ((tasks[current_flag] > 0))
			break;
		if (get_live_tareas() == 0)
			break;
	}
}

char get_live_tareas(){
	int i;
	char live_tareas = 0;
	for (i = 0; i < CANT_TAREAS; ++i)
		if (tasks[i] > 0)
			live_tareas++;
	return live_tareas;
}

unsigned char is_alive(){
	if (tasks[current_flag] > 0)
		return true;
	return false;
}