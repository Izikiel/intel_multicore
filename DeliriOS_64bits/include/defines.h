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

/*Paginacion PAE*/
#define KERNEL_PML4T_POINTER 0x0000000000040000
#define KERNEL_STACK_PTR 0x0000000000027000

/* -------------------------------------------------------------------------- */

#endif  /* !__DEFINES_H__ */
