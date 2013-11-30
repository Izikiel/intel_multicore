/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de estructuras para administrar tareas
*/

#include "tss.h"

tss tarea_inicial;
tss tarea_idle;

tss tss_navios[CANT_TAREAS];
tss tss_bandera;

void tss_inicializar() {

	gdt[23] = (gdt_entry) {  // definir tss tarea inicial
        .limit_0_15 = 104,
        .type = 0x9,
        .s = 0x0,
        .dpl = 0x0,
        .g = 0x1,
        .base_0_15 = (short) (0xFFFF & ((int) &tarea_inicial)),
        .base_23_16 =(char)  (0xFF0000 & ((int) &tarea_inicial)) >> 16,
        .base_31_24 =(char)  (0xFF000000 & ((int) &tarea_inicial)) >> 24,
        .p = 0x1,
        .avl = 0x0
    };

	gdt[24] = (gdt_entry) {  // definir tss tarea idle
        .limit_0_15 = 104,
        .type = 0x9,
        .s = 0x0,
        .dpl = 0x0,
        .g = 0x1,
        .base_0_15 = (short) (0xFFFF & ((int) &tarea_idle)),
        .base_23_16 =(char)  (0xFF0000 & ((int) &tarea_idle)) >> 16,
        .base_31_24 =(char)  (0xFF000000 & ((int) &tarea_idle)) >> 24,
        .p = 0x1
    };

    int i;

    for (i = 0; i < CANT_TAREAS; ++i)
    {
    	gdt[25 + i] = (gdt_entry) {
	        .limit_0_15 = 104,
	        .type = 0x9,
	        .s = 0x0,
	        .dpl = 0x0,
	        .g = 0x1,
	        .base_0_15 = (short) (0xFFFF & ((int) &tss_navios[i])),
	        .base_23_16 =(char)  (0xFF0000 & ((int) &tss_navios[i])) >> 16,
	        .base_31_24 =(char)  (0xFF000000 & ((int) &tss_navios[i])) >> 24,
	        .p = 0x1
    	};
    }

    gdt[25 + CANT_TAREAS] = (gdt_entry) {
	        .limit_0_15 = 104,
	        .type = 0x9,
	        .s = 0x0,
	        .dpl = 0x0,
	        .g = 0x1,
	        .base_0_15 = (short) (0xFFFF & ((int) &tss_bandera)),
	        .base_23_16 =(char)  (0xFF0000 & ((int) &tss_bandera)) >> 16,
	        .base_31_24 =(char)  (0xFF000000 & ((int) &tss_bandera)) >> 24,
	        .p = 0x1
    	};

	tarea_idle = (tss){
	    .cr3 = 0x27000,
	    .eip = TASK_CODE,
	    .eflags = 0x202,
	    .esp = 0x2C000,
	    .ebp = 0x2C000,
	    .es = 0xa0,
	    .cs = 0x90,
	    .ss = 0xa0,
	    .ds = 0xa0,
	    .fs = 0xa0,
	    .gs = 0xa0,
	    .iomap = 0xFFFF
	};
	for (i = 0; i < CANT_TAREAS; i++)
	{
		/*
		 * Estructura de la tarea :
		 *
		 *  0x40000000 - 0x40001000 : codigo tarea          (   5 KB)
		 *  0x40001000 - 0x40001400 : area de banderas      (   1 KB)
		 *  0x40001400 - 0x40001c00 : pila tarea            (   2 KB)
		 *  0x40001c00 - 0x40001ffc : pila bandera          (1020 B )
		 *  0x40001ffc - 0x40002000 : dir. funcion bandera  (   4 B )
		 */

		tss_navios[i] = (tss) {
			        .esp0 = 0x35000 + i * 0x5000,
			        .ss0 = 0xa0,
			        .cr3 = 0x30000 + i * 0x5000,
			        .eip = TASK_CODE,
			        .eflags = 0x202,
			        .esp = TASK_CODE + 0x1C00,
			        .ebp = TASK_CODE + 0x1C00,
			        .es = 0xa8 | 3, //datos de usuario
			        .cs = 0x98 | 3, //codigo usuario
			        .ss = 0xa8 | 3, // el 3 es por el rpl
			        .ds = 0xa8 | 3,
			        .fs = 0xa8 | 3,
			        .gs = 0xa8 | 3,
			        .iomap = 0xFFFF
			    };
	}
}

void setear_tss_bandera(){
	unsigned int bandera_address = TASK_CODE + *((unsigned int *)(TASK_CODE + 0x1FFC));
    tss_bandera = (tss) {
			        .esp0 = 0x70000, //uso una sola para todas las banderas, lejos de la memoria de las tareas
			        .ss0 = 0xa0,
			        .cr3 = rcr3(), // setear dentro de la interrupcion
			        .eip = bandera_address, // dereferenciar como puntero dentro de la interrupcion
			        .eflags = 0x202,
			        .esp = TASK_CODE + 0x1FFB,
			        .ebp = TASK_CODE + 0x1FFB, // consultar
			        .es = 0xa8 | 3, //datos de usuario
			        .cs = 0x98 | 3, //codigo usuario
			        .ss = 0xa8 | 3, // el 3 es por el rpl
			        .ds = 0xa8 | 3,
			        .fs = 0xa8 | 3,
			        .gs = 0xa8 | 3,
			        .iomap = 0xFFFF
			    };
}
