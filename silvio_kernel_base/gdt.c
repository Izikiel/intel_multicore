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
    /* [1]Descriptor de codigo de nivel 0 */
    [GDT_IDX_SEGCODE_LEVEL0_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x09,           /* type: 9h-1001b execute only, accessed  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x00,           /* descriptor privilege level: nivel 0 -> kernel*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x0F,           /* limit[16:19] = 0x0F */
        (unsigned char)     0x00,           /* available for use by system software */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    },
    /* [2]Descriptor de datos de nivel 0 */
    [GDT_IDX_SEGDATA_LEVEL0_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x02,           /* type: 2h-0010b data read write  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x00,           /* descriptor privilege level: nivel 0 -> kernel*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x0F,           /* limit[16:19] = 0x0F */
        (unsigned char)     0x00,           /* available for use by system software */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    }
};

gdt_descriptor GDT_DESC = {
    sizeof(gdt) - 1,
    (unsigned int) &gdt
};

