/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================

    Definiciones globales del sistema.
*/

#ifndef __DEFINES_H__
#define __DEFINES_H__

/* Bool */
/* -------------------------------------------------------------------------- */
#define TRUE                    0x00000001
#define FALSE                   0x00000000


/* Misc */
/* -------------------------------------------------------------------------- */
#define CANT_TAREAS             8

#define TAMANO_PAGINA           0x00001000

#define TASK_SIZE               2 * 4096


/* Indices en la gdt */
/* -------------------------------------------------------------------------- */
#define GDT_IDX_NULL_DESC           0
//segmentos del ejercicio1
#define GDT_IDX_SEGCODE_LEVEL0_DESC 18
#define GDT_IDX_SEGDATA_LEVEL0_DESC 19
#define GDT_IDX_SEGCODE_LEVEL3_DESC 20
#define GDT_IDX_SEGDATA_LEVEL3_DESC 21
#define GDT_IDX_SEGVIDEO_LEVEL0_DESC 22

//segmentos de tareas
#define GDT_IDX_INIT_TASK_DESC	 	23
#define GDT_IDX_IDLE_TASK_DESC 		24

#define GDT_IDX_TASK_1a_DESC 		25
#define GDT_IDX_TASK_1b_DESC 		26

#define GDT_IDX_TASK_2a_DESC 		27
#define GDT_IDX_TASK_2b_DESC 		28

#define GDT_IDX_TASK_3a_DESC 		29
#define GDT_IDX_TASK_3b_DESC 		30

#define GDT_IDX_TASK_4a_DESC 		31
#define GDT_IDX_TASK_4b_DESC 		32

#define GDT_IDX_TASK_5a_DESC 		33
#define GDT_IDX_TASK_5b_DESC 		34

#define GDT_IDX_TASK_6a_DESC 		35
#define GDT_IDX_TASK_6b_DESC 		36

#define GDT_IDX_TASK_7a_DESC 		37
#define GDT_IDX_TASK_7b_DESC 		38

#define GDT_IDX_TASK_8a_DESC 		39
#define GDT_IDX_TASK_8b_DESC 		40

/* Direcciones de memoria */
/* -------------------------------------------------------------------------- */
#define BOOTSECTOR              0x00001000 /* direccion fisica de comienzo del bootsector (copiado) */
#define KERNEL                  0x00001200 /* direccion fisica de comienzo del kernel */
#define VIDEO                   0x000B8000 /* direccion fisica del buffer de video */

/* Direcciones virtuales de código, pila y datos */
/* -------------------------------------------------------------------------- */
#define TASK_CODE               0x40000000 /* direccion virtual del codigo */
#define FLAG_CODE 				0x40001FFC	/* direccion virtual del codigo */

#define TASK_IDLE_CODE          0x40000000 /* direccion virtual del codigo de la tarea idle */
#define TASK_IDLE_STACK         0x003D0000 /* direccion virtual de la pila de la tarea idle */

#define TASK_ANCLA              0x40002000
#define TASK_ANCLA_FIS          0x00000000

#define TASK_VIRTUAL_P1			0x40000000
#define TASK_VIRTUAL_P2			0x40001000
#define TASK_VIRTUAL_P3			0x40002000

#define AREA_TIERRA_INICIO      0x00000000  /* 0.0 MB     */
#define AREA_TIERRA_FIN         0x000FFFFF  /* 1.0 MB - 1 */
#define AREA_MAR_INICIO         0x00100000  /* 1.0 MB     */
#define AREA_MAR_FIN            0x0077FFFF  /* 7.5 MB - 1 */

/* Direcciones fisicas de codigos */
/* -------------------------------------------------------------------------- */
/* En estas direcciones estan los códigos de todas las tareas. De aqui se
 * copiaran al destino indicado por TASK_<i>_CODE_ADDR.
 */

//no las uso, ver mmu.h, indexo con offsets a partir de 0x00010000
//#define TASK_1_CODE_SRC_ADDR    0x00010000
//#define TASK_2_CODE_SRC_ADDR    0x00012000
//#define TAKS_3_CODE_SRC_ADDR    0x00014000
//#define TASK_4_CODE_SRC_ADDR    0x00016000
//#define TASK_5_CODE_SRC_ADDR    0x00018000
//#define TASK_6_CODE_SRC_ADDR    0x0001A000
//#define TASK_7_CODE_SRC_ADDR    0x0001C000
//#define TASK_8_CODE_SRC_ADDR    0x0001E000
//
//#define TASK_IDLE_CODE_SRC_ADDR 0x00020000

#endif  /* !__DEFINES_H__ */
