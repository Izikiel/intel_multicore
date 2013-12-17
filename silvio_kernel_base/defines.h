#ifndef __DEFINES_H__
#define __DEFINES_H__

/* Indices en la gdt */
/* -------------------------------------------------------------------------- */
#define GDT_IDX_NULL_DESC           0
#define GDT_IDX_SEGCODE_LEVEL0_DESC 1
#define GDT_IDX_SEGDATA_LEVEL0_DESC 2

/* Direcciones de memoria */
/* -------------------------------------------------------------------------- */
#define BOOTSECTOR              0x00001000 /* direccion fisica de comienzo del bootsector (copiado) */
#define KERNEL                  0x00001200 /* direccion fisica de comienzo del kernel */
#define VIDEO                   0x000B8000 /* direccion fisica del buffer de video */

#define KERNEL_PAGEDIR_POINTER 0x00027000
#define KERNEL_FIRST_PAGETABLE_POINTER 0x00028000
#define KERNEL_SECOND_PAGETABLE_POINTER 0x00029000

/* -------------------------------------------------------------------------- */

#endif  /* !__DEFINES_H__ */
