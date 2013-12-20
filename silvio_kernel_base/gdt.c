#include "gdt.h"

gdt_entry gdt[GDT_COUNT] = {
    /* Descriptor nulo*/
    /* Offset = 0x00 */
    [GDT_IDX_NULL_DESC] = {
        .limit_0_15 = 0x0000,       /* limit[0:15]  */
        .base_0_15 = 0x0000,        /* base[0:15]   */
        .base_23_16 = 0x00,         /* base[23:16]  */
        .type = 0x00,               /* type         */
        .s = 0x00,                  /* s            */
        .dpl = 0x00,                /* dpl          */
        .p = 0x00,                  /* p            */
        .limit_16_19 = 0x00,        /* limit[16:19] */
        .avl = 0x00,                /* avl          */
        .l = 0x00,                  /* l            */
        .db = 0x00,                 /* db           */
        .g = 0x00,                  /* g            */
        .base_31_24 = 0x00          /* base[31:24]  */
    },
    /* [1]Descriptor de codigo de nivel 0 en 32 bits! */
    [GDT_IDX_SEGCODE_LEVEL0_DESC_32] = {
        .limit_0_15 = 0xFFFF,       /* limit[0:15] = 0xFFFF  */
        .base_0_15 = 0x0000,        /* base[0:15]:0x00   */
        .base_23_16 = 0x00,         /* base[23:16]:0x00  */
        .type = 0x08,               /* type: 8h-1000b execute only  */
        .s = 0x01,                  /* s: 1 code or data  */
        .dpl = 0x00,                /* descriptor privilege level: nivel 0 -> kernel*/
        .p = 0x01,                  /* segment present :si, presente y puede ser usado*/
        .limit_16_19 = 0x0F,        /* limit[16:19] = 0x0F */
        .avl = 0x00,                /* available for use by system software */
        .l = 0x00,                  /* l: bit code segment */
        .db = 0x01,                 /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        .g = 0x01,                  /* g:1 granularidad 4K */
        .base_31_24 = 0x00          /* base[31:24]  */
    },/* [2]Descriptor de codigo de nivel 0 en 64 bits! */
    [GDT_IDX_SEGCODE_LEVEL0_DESC_64] = {
        .limit_0_15 = 0xFFFF,       /* limit[0:15] = 0xFFFF  */
        .base_0_15 = 0x0000,        /* base[0:15]:0x00   */
        .base_23_16 = 0x00,         /* base[23:16]:0x00  */
        .type = 0x08,               /* type: 8h-1000b execute only  */
        .s = 0x01,                  /* s: 1 code or data  */
        .dpl = 0x00,                /* descriptor privilege level: nivel 0 -> kernel*/
        .p = 0x01,                  /* segment present :si, presente y puede ser usado*/
        .limit_16_19 = 0x0F,        /* limit[16:19] = 0x0F */
        .avl = 0x00,                /* available for use by system software */
        .l = 0x01,                  /* l: bit code segment 64 bits mode ON! */
        .db = 0x00,                 /* 64 bits mode on!*/
        .g = 0x01,                  /* g:1 granularidad 4K */
        .base_31_24 = 0x00          /* base[31:24]  */
    },
    /* [3]Descriptor de datos de nivel 0 */
    [GDT_IDX_SEGDATA_LEVEL0_DESC] = {
        .limit_0_15 = 0xFFFF,       /* limit[0:15] = 0xFFFF  */
        .base_0_15 = 0x0000,        /* base[0:15]:0x00   */
        .base_23_16 = 0x00,         /* base[23:16]:0x00  */
        .type = 0x02,               /* type: 2h-0010b data read write  */
        .s = 0x01,                  /* s: 1 code or data  */
        .dpl = 0x00,                /* descriptor privilege level: nivel 0 -> kernel*/
        .p = 0x01,                  /* segment present :si, presente y puede ser usado*/
        .limit_16_19 = 0x0F,        /* limit[16:19] = 0x0F */
        .avl = 0x00,                /* available for use by system software */
        .l = 0x00,                  /* l: bit code segment*/
        .db = 0x01,                 /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        .g = 0x01,                  /* g:1 granularidad 4K */
        .base_31_24 = 0x00          /* base[31:24]  */
    }
};

gdt_descriptor GDT_DESC = {
    .gdt_length = sizeof(gdt) - 1,
    .gdt_addr = (uint64_t) &gdt
};