#include <timer.h>
#include <irq.h>
#include <proc.h>
#include <asserts.h>

static char clk[] = {'-','\\','|','/'};
static uchar clk_place;

#define MAX_POLLS 32
static volatile uint * polls[MAX_POLLS];
static volatile uint poll_count;

//Handler de timer tick.
void timer_tick(uint irq_code, gen_regs regs)
{
    uint flags = irq_cli(); 
	scrn_pos_printf(VIDEO_HEIGHT-1,
					VIDEO_WIDTH-1-strlen("TICK _"),
					"TICK: %c",clk[clk_place]);
	clk_place = (clk_place+1)%4;
	//Actualizamos todos los polls para que entonces los que estan durmiendo
	//sepan cuanto tiempo paso.
	for(uint i = 0; i < poll_count; i++){
		if(*polls[i] > 0){
			*polls[i] = *polls[i]-1;
		}
	}
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

//Inicializa un sleep de la cantidad de milisegundos dada.
void core_sleep(uint miliseconds)
{
	//Agregar mi contador a los polls
	uint eflags = irq_cli();
	volatile uint status = miliseconds;	

	fail_if(poll_count == MAX_POLLS);
	polls[poll_count++] = &status;
	irq_sti(eflags);

	//Esperar a que termine de loopear
	for(bool done = false; !done;){
		eflags = irq_cli();
		if(status == 0){
			done = true;
		}
		irq_sti(eflags);
	}
}
