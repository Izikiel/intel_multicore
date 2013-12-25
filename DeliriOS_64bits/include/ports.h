#ifndef __PIC_H__
#define __PIC_H__
#include <types.h>

void resetear_pic(void);
void habilitar_pic();
void deshabilitar_pic();

//Escribe al CMOS
extern void cmos_writeb(uint8_t port, uint8_t data);

__attribute__((always_inline)) void outb(uint8_t port, uint8_t data);

__attribute__((always_inline)) void fin_intr_pic1();
__attribute__((always_inline)) void fin_intr_pic2();

#endif	/* !__PIC_H__ */
