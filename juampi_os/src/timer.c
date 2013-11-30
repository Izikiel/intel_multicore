#include <timer.h>
#include <irq.h>
#include <proc.h>

static char clk[] = {'-','\\','|','/'};
static uchar clk_place;

//Handler de timer tick.
void timer_tick(uint irq_code, gen_regs regs)
{
    uint flags = irq_cli(); 
	scrn_pos_printf(VIDEO_HEIGHT-1,
					VIDEO_WIDTH-1-strlen("TICK _"),
					"TICK: %c",clk[clk_place]);
	clk_place = (clk_place+1)%4;
	irq_sti(flags);
}

//Modificar frequencia del PIT (Programmable
//Interrupt Controller) para que la frecuencia
//sea la deseada
void init_timer(uint freq)
{
	uint div = 1193180/freq;
	outb(0x43,0x36);
	uchar l = (uchar)(div & 0xFF);
	uchar h = (uchar)((div>>8)&0xFF);
	outb(0x40,l);
	outb(0x40,h);

	register_irq_handler(timer_tick,0x20);	
}
