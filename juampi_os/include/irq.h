#ifndef __INTERRUPTS_H
#define __INTERRUPTS_H

#include <scrn.h>
#include <types.h>
#include <idt.h>
#include <ports.h>
#include <proc.h>

//Tipo de los callbacks
typedef void (*irq_handler) (uint,gen_regs);

//Remapeo del PIC
extern void remap_pic();

//Cargar los handlers de irq y amigas
extern void irq_init_handlers();

//Handler general interrupcciones por PIC
extern void irq_common_handler(gen_regs,uint,int_trace);

//Handlers para prender y apagar interrupciones
//irq_sti decide si tiene que activar o no segun eflags
extern void irq_sti(uint eflags);
//Activa si o si interrupciones
extern void irq_sti_force();
//Desactiva interrupciones, devuelve el eflags anterior
extern uint irq_cli();

//Registrar handler
extern void register_irq_handler(irq_handler e, uint code);

//Maxima cantidad de interrupciones que maneja el sistema
#define MAX_INTS 64

#endif
