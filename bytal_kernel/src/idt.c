/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de las rutinas de atencion de interrupciones
*/

#include "idt.h"

idt_entry idt[255] = { };

idt_descriptor IDT_DESC = {
    sizeof(idt) - 1,
    (unsigned int) &idt
};


/*
    La siguiente es una macro de EJEMPLO para ayudar a armar entradas de
    interrupciones. Para usar, descomentar y completar CORRECTAMENTE los
    atributos y el registro de segmento. Invocarla desde idt_inicializar() de
    la siguiene manera:

    void idt_inicializar() {
        IDT_ENTRY(0);
        ...
        IDT_ENTRY(19);

        ...
    }
*/

#define IDT_ENTRY(numero)\
    idt[numero].offset_0_15 = (unsigned short) ((unsigned int)(&_isr ## numero) & (unsigned int) 0xFFFF);\
    idt[numero].segsel = (unsigned short) (18<<3);\
    idt[numero].attr = (unsigned short) 0x8E00;\
    idt[numero].offset_16_31 = (unsigned short) ((unsigned int)(&_isr ## numero) >> 16 & (unsigned int) 0xFFFF);


#define syscall(numero)\
    idt[numero].offset_0_15 = (unsigned short) ((unsigned int)(&_isr ## numero) & (unsigned int) 0xFFFF);\
    idt[numero].segsel = (unsigned short) (18<<3);\
    idt[numero].attr = (unsigned short) 0xEE00;\
    idt[numero].offset_16_31 = (unsigned short) ((unsigned int)(&_isr ## numero) >> 16 & (unsigned int) 0xFFFF);

void idt_inicializar() {
    IDT_ENTRY(0);   //Division
    IDT_ENTRY(1);   //Division
    IDT_ENTRY(2);   //Division
    IDT_ENTRY(3);   //breakpoint
    IDT_ENTRY(4);   //Division
    IDT_ENTRY(5);   //Division
    IDT_ENTRY(6);   //Division
    IDT_ENTRY(7);   //breakpoint
    IDT_ENTRY(8);   //Division
    IDT_ENTRY(9);   //Division
    IDT_ENTRY(10);   //Division
    IDT_ENTRY(11);   //breakpoint
    IDT_ENTRY(12);   //Division
    IDT_ENTRY(13);   //Division
    IDT_ENTRY(14);   //Division
    IDT_ENTRY(15);   //breakpoint
    IDT_ENTRY(16);   //Division
    IDT_ENTRY(17);   //Division
    IDT_ENTRY(18);   //Division
    IDT_ENTRY(19);   //breakpoint

    IDT_ENTRY(32);   //reloj
    IDT_ENTRY(33);   //teclado

    syscall(80);    //syscall
    syscall(102);   //syscall
}