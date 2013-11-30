#include <irq.h>
#include <idt.h>
#include <utils.h>
#include <scrn.h>
#include <exception.h>

//Punteros con las funciones de atencion de interrupcciones.
static irq_handler irq_handlers[MAX_INTS];

//Envia los mensajes para que se remapee el pic.
void remap_pic()
{
	outb(0x20,0x11);
	outb(0xA0,0x11);
	outb(0x21,0x20);
	outb(0xA1,0x28);
	outb(0x21,0x04);
	outb(0xA1,0x02);
	outb(0x21,0x01);
	outb(0xA1,0x01);
	outb(0x21,0x00);
	outb(0xA1,0x00);
}

static void irq_unknown_handler(uint irq_code,gen_regs regs)
{
	uchar row = scrn_getrow(), col = scrn_getcol();
	scrn_setcursor(VIDEO_HEIGHT-1,0);
	scrn_printf("Interrupccion %u desconocida",irq_code,irq_code);
	scrn_setcursor(row,col);
}

//Registra un handler para una interrupcion
void register_irq_handler(irq_handler ih, uint code)
{
	if(code < MAX_INTS) {
		irq_handlers[code] = ih;
	} else {
		kernel_panic("Interrupcion invalida");
	}
}

void irq_init_handlers()
{
	memset(irq_handlers,0,sizeof(irq_handlers));

	for(int i = 0; i < MAX_INTS; i++) {
		irq_handlers[i] = irq_unknown_handler;
	}
}

#define IF_BIT 9
inline void irq_sti(uint eflags)
{
	//Si originalmente habia interrupciones las volvemos
	//a habilitar
	if(eflags & (1 << IF_BIT)) {
		__asm__ __volatile__("sti");
	}
}

inline void irq_sti_force()
{
	__asm__ __volatile__("sti");
}

inline uint irq_cli()
{
	uint eflags;
	__asm__ __volatile__("pushf\n\t"
	                     "pop %%eax\n\t"
	                     "mov %%eax, %0"
	                     : "=r"(eflags)
	                     :: "eax");
	__asm__ __volatile__("cli");
	return eflags;
}

//Handler comun para las interrupciones por hardware: Estas vienen de
//los pines del PIC de la CPU
void irq_common_handler(gen_regs regs, uint irq_code, int_trace trace)
{
	if(irq_code == 7+32) {
		//Spurious interrupt. Si no esta activado
		//el bit 7 de interrupcion real, retornamos.
		outb(0x20,0x0B);
		uchar irr = inb(0x20);
		if(!(irr & 0x80)) {
			return;
		}
	}
	if(irq_code >= 40) {
		//Le decimos al PIC master que procesamos la interrupcion.
		outb(0xA0, 0x20);
	}
	outb(0x20, 0x20);
	
	if(irq_handlers[irq_code]) {
		//Levantamos el handler de verdad
		irq_handler handler = irq_handlers[irq_code];
		handler(irq_code,regs);
	}
}

