#ifndef __PIC_H__
#define __PIC_H__

void resetear_pic(void);
void habilitar_pic();
void deshabilitar_pic();

__attribute__((always_inline)) void outb(int port, unsigned char data);

__attribute__((always_inline)) void fin_intr_pic1();
__attribute__((always_inline)) void fin_intr_pic2();

#endif	/* !__PIC_H__ */
