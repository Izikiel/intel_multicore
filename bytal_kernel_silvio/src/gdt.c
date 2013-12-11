/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de la tabla de descriptores globales
*/

#include "gdt.h"
#include "tss.h"
//la base de los segmentos va a ser siempre 0x0
//para direccionar arriba de 1mb tengo que usar granularidad a 4k.
//1.75gb => 1792mb => 1835008kb => divido por 4 y me da la cantidad de segmentos de 4k en 1.75gb => 458752 => pasando a hexa => 6FFFFh(20 bits piola)

//declaro 4 segmentos flat a partir del indice 18 inclusive
//y uno mas para video

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
    /* [18]Descriptor de codigo de nivel 0 */
    [GDT_IDX_SEGCODE_LEVEL0_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x09,           /* type: 9h-1001b execute only, accessed  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x00,           /* descriptor privilege level: nivel 0 -> kernel*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x06,           /* limit[16:19] = 0x06 */
        (unsigned char)     0x00,           /* available for use by system software: no */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    },
    /* [19]Descriptor de datos de nivel 0 */
    [GDT_IDX_SEGDATA_LEVEL0_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x02,           /* type: 2h-0010b data read write  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x00,           /* descriptor privilege level: nivel 0 -> kernel*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x06,           /* limit[16:19] = 0x06 */
        (unsigned char)     0x00,           /* available for use by system software: no */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    },    
    /* [20]Descriptor de codigo de nivel 3 */
    [GDT_IDX_SEGCODE_LEVEL3_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x09,           /* type: 9h-1001b execute only, accessed  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x03,           /* descriptor privilege level: nivel 3 -> usuario*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x06,           /* limit[16:19] = 0x06 */
        (unsigned char)     0x01,           /* available for use by system software: si, level 3 => usuario */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    },
    /* [21]Descriptor de datos de nivel 3 */
    [GDT_IDX_SEGDATA_LEVEL3_DESC] = (gdt_entry) {
        (unsigned short)    0xFFFF,         /* limit[0:15] = 0xFFFF  */
        (unsigned short)    0x0000,         /* base[0:15]:0x00   */
        (unsigned char)     0x00,           /* base[23:16]:0x00  */
        (unsigned char)     0x02,           /* type: 2h-0010b data read write  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x03,           /* descriptor privilege level: nivel 3 -> usuario*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x06,           /* limit[16:19] = 0x06 */
        (unsigned char)     0x01,           /* available for use by system software: si, level 3 => usuario */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x01,           /* g:1 granularidad 4K */
        (unsigned char)     0x00,           /* base[31:24]  */
    },
    /* [22]Descriptor de video de nivel 0 */
    [GDT_IDX_SEGVIDEO_LEVEL0_DESC] = (gdt_entry) {
        (unsigned short)    0x0F9F,         /* limit[0:15] = 0x0F9F  */
        (unsigned short)    0x8000,         /* base[0:15]:0x8000 - memoria de video   */
        (unsigned char)     0x0B,           /* base[23:16]:0x0B - completa memoria de video en 0xB8000  */
        (unsigned char)     0x02,           /* type: 2h-0010b data read write  */
        (unsigned char)     0x01,           /* s: 1 code or data  */
        (unsigned char)     0x00,           /* descriptor privilege level: nivel 0 -> kernel - va a ser video solo usado por el kernel*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x00,           /* limit[16:19] = 0x00 */
        (unsigned char)     0x00,           /* available for use by system software: no */
        (unsigned char)     0x00,           /* l: bit code segment, NO, estoy en 32 bits */
        (unsigned char)     0x01,           /* default operation size: 32 bits, osea 1 , de esta forma se interpretaran las instrucciones*/
        (unsigned char)     0x00,           /* g:0 granularidad 1Byte */
        (unsigned char)     0x00,           /* base[31:24]  */
    }    
};

gdt_descriptor GDT_DESC = {
    sizeof(gdt) - 1,
    (unsigned int) &gdt
};

//hay que hacerlo dinamico porque si. yo me entiendo, estatico no compila porque uso variables y no constantes
void inicializar_gdt_tareas(){
     /* [23]Descriptor de la tarea init */
    gdt[GDT_IDX_INIT_TASK_DESC] = (gdt_entry) {
        (unsigned short)    INIT_TSS_DESC.gdt_length, /* limit[0:15] = BLACK MAGIC  */ /* limit[0:15] = 0x67 (minimo limite para tss)  */
        (unsigned short)    INIT_TSS_DESC.gdt_addr,   /* base[0:15]: corta la primera parte del int*/
        (unsigned char)     INIT_TSS_DESC.gdt_addr >> 16,           /* base[23:16]:0x0B - mata los primeros 2 bytes y castea a char  */
        (unsigned char)     0x09,           /* type: 10B1b -> bit busy en 0 */
        (unsigned char)     0x00,           /* s: 0 TSS  */
        (unsigned char)     0x00,           /* descriptor privilege level: 0*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x00,           /* limit[16:19] = 0x00  supongo que como es 69 (minimo) entonces esto queda en cero*/
        (unsigned char)     0x00,           /* available for use by system software: no */
        (unsigned char)     0x00,           /* 0 */
        (unsigned char)     0x00,           /* 0 */
        (unsigned char)     0x00,           /* g:0 granularidad 1Byte */
        (unsigned char)     INIT_TSS_DESC.gdt_addr >> 24,/* base[31:24] mata los primeros 3 bytes y castea a char */
    };

    /* [24]Descriptor de la tarea idle */
    gdt[GDT_IDX_IDLE_TASK_DESC] = (gdt_entry) {
        (unsigned short)    IDLE_TSS_DESC.gdt_length, /* limit[0:15] = BLACK MAGIC  */ /* limit[0:15] = 0x67 (minimo limite para tss)  */
        (unsigned short)    IDLE_TSS_DESC.gdt_addr,   /* base[0:15]: corta la primera parte del int*/
        (unsigned char)     IDLE_TSS_DESC.gdt_addr >> 16,           /* base[23:16]:0x0B - mata los primeros 2 bytes y castea a char  */
        (unsigned char)     0x09,           /* type: 10B1b -> bit busy en 0 */
        (unsigned char)     0x00,           /* s: 0 TSS  */
        (unsigned char)     0x00,           /* descriptor privilege level: 0*/
        (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
        (unsigned char)     0x00,           /* limit[16:19] = 0x00  supongo que como es 69 (minimo) entonces esto queda en cero*/
        (unsigned char)     0x00,           /* available for use by system software: no */
        (unsigned char)     0x00,           /* 0 */
        (unsigned char)     0x00,           /* 0 */
        (unsigned char)     0x00,           /* g:0 granularidad 1Byte */
        (unsigned char)     IDLE_TSS_DESC.gdt_addr >> 24,/* base[31:24] mata los primeros 3 bytes y castea a char */
    };

//  segmentos de tareas predefinidos en defines.h, tomando esto, voy a pasarlo a un for
//  #define GDT_IDX_INIT_TASK_DESC      23
//  #define GDT_IDX_IDLE_TASK_DESC      24
//  
//  #define GDT_IDX_TASK_1a_DESC        25
//  #define GDT_IDX_TASK_1b_DESC        26
//  
//  #define GDT_IDX_TASK_2a_DESC        27
//  #define GDT_IDX_TASK_2b_DESC        28
//  
//  #define GDT_IDX_TASK_3a_DESC        29
//  #define GDT_IDX_TASK_3b_DESC        30
//  
//  #define GDT_IDX_TASK_4a_DESC        31
//  #define GDT_IDX_TASK_4b_DESC        32
//  
//  #define GDT_IDX_TASK_5a_DESC        33
//  #define GDT_IDX_TASK_5b_DESC        34
//  
//  #define GDT_IDX_TASK_6a_DESC        35
//  #define GDT_IDX_TASK_6b_DESC        36
//  
//  #define GDT_IDX_TASK_7a_DESC        37
//  #define GDT_IDX_TASK_7b_DESC        38
//  
//  #define GDT_IDX_TASK_8a_DESC        39
//  #define GDT_IDX_TASK_8b_DESC        40

    //usando como base GDT_IDX_TASK_1a_DESC = 25 indexo (GDT_IDX_TASK_1a_DESC + 2*taskNumber) las entradas de TSS Task y con (GDT_IDX_TASK_1a_DESC + 2*taskNumber + 1) las entradas tss de flag
    int taskNumber = 0;
    int effectiveTaskIndex = 0;
    int effectiveFlagIndex = 0;
    for(taskNumber=0;taskNumber<8;taskNumber++){
        //tareas entre 0 a 7 para inicializar
        //calculo los indices
        effectiveTaskIndex = GDT_IDX_TASK_1a_DESC + 2*taskNumber;
        effectiveFlagIndex = GDT_IDX_TASK_1a_DESC + 2*taskNumber + 1;

        gdt[effectiveTaskIndex] = (gdt_entry) {
            (unsigned short)    TASKS_TSS_DESC[taskNumber].gdt_length, /* limit[0:15] = BLACK MAGIC  */ /* limit[0:15] = 0x67 (minimo limite para tss)  */
            (unsigned short)    TASKS_TSS_DESC[taskNumber].gdt_addr,   /* base[0:15]: corta la primera parte del int*/
            (unsigned char)     TASKS_TSS_DESC[taskNumber].gdt_addr >> 16,           /* base[23:16]:0x0B - mata los primeros 2 bytes y castea a char  */
            (unsigned char)     0x09,           /* type: 10B1b -> bit busy en 0 */
            (unsigned char)     0x00,           /* s: 0 TSS  */
            (unsigned char)     0x00,           /* descriptor privilege level: 0*/
            (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
            (unsigned char)     0x00,           /* limit[16:19] = 0x00  supongo que como es 69 (minimo) entonces esto queda en cero*/
            (unsigned char)     0x00,           /* available for use by system software: no */
            (unsigned char)     0x00,           /* 0 */
            (unsigned char)     0x00,           /* 0 */
            (unsigned char)     0x00,           /* g:0 */
            (unsigned char)     TASKS_TSS_DESC[taskNumber].gdt_addr >> 24,/* base[31:24] mata los primeros 3 bytes y castea a char */
        };

        gdt[effectiveFlagIndex] = (gdt_entry) {
            (unsigned short)    FLAGS_TSS_DESC[taskNumber].gdt_length, /* limit[0:15] = BLACK MAGIC  */ /* limit[0:15] = 0x67 (minimo limite para tss)  */
            (unsigned short)    FLAGS_TSS_DESC[taskNumber].gdt_addr,   /* base[0:15]: corta la primera parte del int*/
            (unsigned char)     FLAGS_TSS_DESC[taskNumber].gdt_addr >> 16,           /* base[23:16]:0x0B - mata los primeros 2 bytes y castea a char  */
            (unsigned char)     0x09,           /* type: 10B1b -> bit busy en 0 */
            (unsigned char)     0x00,           /* s: 0 TSS  */
            (unsigned char)     0x00,           /* descriptor privilege level: 0*/
            (unsigned char)     0x01,           /* segment present :si, presente y puede ser usado*/
            (unsigned char)     0x00,           /* limit[16:19] = 0x00  supongo que como es 69 (minimo) entonces esto queda en cero*/
            (unsigned char)     0x00,           /* available for use by system software: no */
            (unsigned char)     0x00,           /* 0 */
            (unsigned char)     0x00,           /* 0 */
            (unsigned char)     0x00,           /* g:0 */
            (unsigned char)     FLAGS_TSS_DESC[taskNumber].gdt_addr >> 24,/* base[31:24] mata los primeros 3 bytes y castea a char */
        };


    }
}
