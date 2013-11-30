#ifndef __ISR_H
#define __ISR_H

#include <types.h>
#include <proc.h>

struct idt_entry{
	ushort offset_l;
	ushort selector;
	uchar __padding__;
	uchar type 	:5;
	uchar dpl	:2;
	uchar p		:1;
	ushort offset_h;
} __attribute__((__packed__));
typedef struct idt_entry idt_entry;

struct idt_entry_flags{
	uchar d 	:1;
	uchar dpl	:2;
	uchar type	:5;
} __attribute__((__packed__));
typedef struct idt_entry_flags idt_entry_flags;

struct idt_desc{
	ushort idt_limit;
	uint idt_base;
} __attribute__ ((__packed__));
typedef struct idt_desc idt_desc;

enum idt_descriptor_type {
	IDT_TASK_GATE 	= 5,
	IDT_INT_GATE 	= 6,
	IDT_TRAP_GATE 	= 7
};
typedef enum idt_descriptor_type idt_descriptor_type;

extern idt_entry idt[];
extern idt_desc IDT_DESC;

//Carga un descriptor de la idt.
extern void idt_load_desc(uint,uint,ushort,idt_entry_flags);
//Levanta la idt de las excepciones para que la use el sistema.
extern void idt_init_exceptions();
extern void idt_init_interrupts();
extern void idt_init_syscalls();
//Carga la idt. Esta en assembler, porque asi tiene que ser. Se encuentra en loader.asm
extern void idt_flush();

//Handlers de excepcion
extern void _isr0();
extern void _isr1();
extern void _isr2();
extern void _isr3();
extern void _isr4();
extern void _isr5();
extern void _isr6();
extern void _isr7();
extern void _isr8();
extern void _isr9();
extern void _isr10();
extern void _isr11();
extern void _isr12();
extern void _isr13();
extern void _isr14();
extern void _isr15();
extern void _isr16();
extern void _isr17();
extern void _isr18();
extern void _isr19();

//Interrupciones
extern void _irq0(); //Tick de reloj
extern void _irq1();
extern void _irq2(); 
extern void _irq3(); 
extern void _irq4(); 
extern void _irq5(); 
extern void _irq6(); 
extern void _irq7(); 
extern void _irq8(); 
extern void _irq9(); 
extern void _irq10();
extern void _irq11();
extern void _irq12();
extern void _irq13();
extern void _irq14();
extern void _irq15();

//Handler de syscalls
extern void _isr0x80();

#endif
