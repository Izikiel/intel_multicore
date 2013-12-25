#ifndef __CONTEXT_MANAGER_H__
#define __CONTEXT_MANAGER_H__

#include <defines.h>
#include <types.h>

//es llamada por la isr cuando salta clock
void notificarRelojTick();
//es llamada por isr cuando se presiona teclado
void notificarTecla(const unsigned char keyCode);

#endif  /* !__CONTEXT_MANAGER_H__*/

