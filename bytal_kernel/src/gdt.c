/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de la tabla de descriptores globales
*/

#include "gdt.h"

gdt_entry gdt[GDT_COUNT] = {
    /* Descriptor nulo*/
    /* Offset = 0x00 */
    [GDT_IDX_NULL_DESC] = (gdt_entry) {
        (unsigned short)    0x0000,         /* limit[0:15]  */
        (unsigned short)    0x0000,         /* base[0:15]   */
        (unsigned char)     0x00,           /* base[23:16]  */
        (unsigned char)     0x00,           /* type         */
        (unsigned char)     0x00,           /* s            */
        (unsigned char)     0x00,           /* dpl          */
        (unsigned char)     0x00,           /* p            */
        (unsigned char)     0x00,           /* limit[16:19] */
        (unsigned char)     0x00,           /* avl          */
        (unsigned char)     0x00,           /* l            */
        (unsigned char)     0x00,           /* db           */
        (unsigned char)     0x00,           /* g            */
        (unsigned char)     0x00,           /* base[31:24]  */
    },
    [18] = {
        .limit_0_15 = 0xFFFF,
        .base_0_15 = 0x0000,
        .base_23_16 = 0x00,
        .type = 0xa,        // codigo
        .s = 0x1,
        .dpl = 0x0,         // kernel
        .p = 0x1,
        .limit_16_19 = 0x6,
        .avl = 0x0,
        .l = 0x0,
        .db = 0x1,
        .g = 0x1,
        .base_31_24 = 0x00
    },
    [19] = {
        .limit_0_15 = 0xFFFF,
        .base_0_15 = 0x0000,
        .base_23_16 = 0x00,
        .type = 0xa,        // codigo
        .s = 0x1,
        .dpl = 0x3,         // usuario
        .p = 0x1,
        .limit_16_19 = 0x6,
        .avl = 0x0,
        .l = 0x0,
        .db = 0x1,
        .g = 0x1,
        .base_31_24 = 0x00
    },
    [20] = {
        .limit_0_15 = 0xFFFF,
        .base_0_15 = 0x0000,
        .base_23_16 = 0x00,
        .type = 0x2,    // datos
        .s = 0x1,
        .dpl = 0x0,     // kernel
        .p = 0x1,
        .limit_16_19 = 0x6,
        .avl = 0x0,
        .l = 0x0,
        .db = 0x1,
        .g = 0x1
    },
    [21] = {
        .limit_0_15 = 0xFFFF,
        .base_0_15 = 0x0000,
        .base_23_16 = 0x00,
        .type = 0x2,    // datos
        .s = 0x1,
        .dpl = 0x3,     // usuario
        .p = 0x1,
        .limit_16_19 = 0x6,
        .avl = 0x0,
        .l = 0x0,
        .db = 0x1,
        .g = 0x1,
        .base_31_24 = 0x00
    },
    [22] = { // definir segmento de video
        .limit_0_15 = 0x0FBF,
        .base_0_15 = 0x8000,
        .base_23_16 = 0x0B,
        .type = 0x2,    // datos
        .s = 0x1,
        .dpl = 0x0,     // kernel
        .p = 0x1,
        .limit_16_19 = 0x0,
        .avl = 0x0,
        .l = 0x0,
        .db = 0x1,
        .g = 0x0,
        .base_31_24 = 0x00
    }

};

gdt_descriptor GDT_DESC = {
    sizeof(gdt) - 1,
    (unsigned int) &gdt
};
