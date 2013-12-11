#include "defines.h"
#include "mmu.h"

#ifndef __CONTEXT_MANAGER_H__
#define __CONTEXT_MANAGER_H__

///es llamada por la isr cuando salta excepcion
void notificarExcepcion(int errorCode, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
///es llamada por la isr cuando salta clock
void notificarRelojTick();
//es llamada por isr cuando se presiona teclado
void notificarTecla(unsigned char keyCode);
//es llamada por mmu cuando se mapea, o desmapea una pagina
void notificarMapeoPagina(unsigned int virtual, unsigned int fisica);
void notificarDesmapeoPagina(unsigned int virtual);
void notificarCambioContexto(pagedir_entry *newDirContext);

//utiles en asmutils
void haltCpu();
void sleepClock();
void jmpToTask(unsigned short int selector);

#endif  /* !__CONTEXT_MANAGER_H__*/

