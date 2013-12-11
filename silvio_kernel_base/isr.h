#ifndef __ISR_H__
#define __ISR_H__

//excepciones:
//las interrupciones 1,9,15 y [20,31] estan reservadas por intel
void _isr0();
//void _isr1();
void _isr2();
void _isr3();
void _isr4();
void _isr5();
void _isr6();
void _isr7();
void _isr8();
//void _isr9();
void _isr10();
void _isr11();
void _isr12();
void _isr13();
void _isr14();
//void _isr15();
void _isr16();
void _isr17();
void _isr18();
void _isr19();

//clock and keyboard interrupts:
void _isr32();
void _isr33();

//system services interrupts
void _isr80();
void _isr102();

#endif  /* !__ISR_H__ */
