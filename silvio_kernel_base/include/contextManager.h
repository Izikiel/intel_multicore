#ifndef __CONTEXT_MANAGER_H__
#define __CONTEXT_MANAGER_H__

#include "defines.h"
#include <stdint.h>

///es llamada por la isr cuando salta excepcion
void notificarExcepcion(uint32_t errorCode, uint64_t FLAGS, uint64_t RDI,
	uint64_t RSI, uint64_t RBP, uint64_t RSP, uint64_t RBX,
	 uint64_t RDX, uint64_t RCX, uint64_t RAX, uint64_t RIP);

///es llamada por la isr cuando salta clock
void notificarRelojTick();
//es llamada por isr cuando se presiona teclado
void notificarTecla(unsigned char keyCode);

#endif  /* !__CONTEXT_MANAGER_H__*/

