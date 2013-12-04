#ifndef __EXCEPTION_H
#define __EXCEPTION_H

#include <types.h>
#include <proc.h>
#include <scrn.h>
#include <irq.h>

typedef void (*exception_handler)(exception_trace);

extern exception_handler exception_handlers[];

//Registra un handler especial
void register_exception_handler(exception_handler,uint);

//Inicializa los handlers de excepcion a un default
void initialize_exception_handlers();

//Pantalla azul clasica
extern void blue_screen(exception_trace);

#endif
