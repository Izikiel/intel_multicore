#ifndef __DEFINES_H__
#define __DEFINES_H__

/* Indices en la gdt */
/* -------------------------------------------------------------------------- */
#define GDT_IDX_NULL_DESC           	0
#define GDT_IDX_SEGCODE_LEVEL0_DESC_32 	1
#define GDT_IDX_SEGCODE_LEVEL0_DESC_64 	2
#define GDT_IDX_SEGDATA_LEVEL0_DESC 	3

/* Direcciones de memoria */
/* -------------------------------------------------------------------------- */
#define VIDEO_MEMORY            0x00000000000B8000 /* direccion fisica del buffer de video */

/*OJO QUE PARA CADA CORE TIENE QUE HABER PILAS DIFERENTES!*/
/*LAS MAPEO ARRIBA DEL PRIMER MEGA*/
#define KERNEL_STACK_PTR_BSP 0x0000000000310000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP1 0x0000000000420000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP2 0x0000000000530000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP3 0x0000000000640000//OJO QUE SON EXPAND DOWN!!

/*Paginacion IA32e -> estructuras arriba del primer mega*/
#define KERNEL_PML4T_POINTER 	0x0000000000740000
#define KERNEL_PDPT_POINTER		0x0000000000841000
#define KERNEL_PDT_POINTER 		0x0000000000942000

/* -------------------------------------------------------------------------- */

#endif  /* !__DEFINES_H__ */
