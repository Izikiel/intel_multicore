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
extern void idt_init_exceptions(void);
extern void idt_init_interrupts(void);
extern void idt_init_syscalls(void);
//Carga la idt. Esta en assembler, porque asi tiene que ser. Se encuentra en loader.asm
extern void idt_flush(void);

//Handlers de excepcion
extern void _isr0(void);
extern void _isr1(void);
extern void _isr2(void);
extern void _isr3(void);
extern void _isr4(void);
extern void _isr5(void);
extern void _isr6(void);
extern void _isr7(void);
extern void _isr8(void);
extern void _isr9(void);
extern void _isr10(void);
extern void _isr11(void);
extern void _isr12(void);
extern void _isr13(void);
extern void _isr14(void);
extern void _isr15(void);
extern void _isr16(void);
extern void _isr17(void);
extern void _isr18(void);
extern void _isr19(void);

//Interrupciones
extern void _irq0(void); //Tick de reloj
extern void _irq1(void);
extern void _irq2(void); 
extern void _irq3(void); 
extern void _irq4(void); 
extern void _irq5(void); 
extern void _irq6(void); 
extern void _irq7(void); 
extern void _irq8(void); 
extern void _irq9(void); 
extern void _irq10(void);
extern void _irq11(void);
extern void _irq12(void);
extern void _irq13(void);
extern void _irq14(void);
extern void _irq15(void);

//Handler de ignorar interrupciones espurias
//	De acuerdo a la APIC spec, los 4 bits de abajo deben estar en 1.
//	Lo demas lo seteamos nosotros. 
#define SPURIOUS_VEC_NUM ((1 << 3) & 0xF)
#define SPURIOUS_INTR_NUM ((SPURIOUS_VEC_NUM << 4) | 0xF)

extern void _irq_ignore_handler(void);

#endif
