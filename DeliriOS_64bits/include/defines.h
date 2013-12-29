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
#define BOOTSECTOR              0x0000000000001000 /* direccion fisica de comienzo del bootsector (copiado) */
#define VIDEO_MEMORY            0x00000000000B8000 /* direccion fisica del buffer de video */

/*OJO QUE PARA CADA CORE TIENE QUE HABER PILAS DIFERENTES!*/
/*LAS MAPEO ARRIBA DEL PRIMER MEGA*/
#define KERNEL_STACK_PTR_BSP 0x0000000000110000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP1 0x0000000000120000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP2 0x0000000000130000//OJO QUE SON EXPAND DOWN!!
#define KERNEL_STACK_PTR_AP3 0x0000000000140000//OJO QUE SON EXPAND DOWN!!

/*Paginacion IA32e -> estructuras arriba del primer mega*/
#define KERNEL_PML4T_POINTER 	0x0000000000140000
#define KERNEL_PDPT_POINTER		0x0000000000141000
#define KERNEL_PDT_POINTER 		0x0000000000142000

/*Malloc config*/
#define DYNAMIC_MEMORY_START 	0x200000//ACTIVO LA ENTREGA DE MEMORIA A PARTIR DEL SEGUNDO MEGA

/* -------------------------------------------------------------------------- */

#endif  /* !__DEFINES_H__ */
