#include <timer.h>
#include <console.h>
#include <irq.h>
#include <asserts.h>
#include <ports.h>

static volatile uint64_t sleep_pool[MAX_INSTANCES];

void initialize_timer(){
	//simulamos "lock" con cli y sti
	irq_cli();

	//segun http://www.osdever.net/bkerndev/Docs/pit.htm
	//The default freq is 18.222Hz tick rate and was used in order
	//for the tick count to cycle at 0.055 seconds.
	//Using a 16-bit timer tick counter, the counter will overflow 
	//and wrap around to 0 once every hour	

	//TODO: empieza a tirar general protection violentamente despues si hago esto
	//por lo tanto lo voy a dejar en default

	////initialize speed (http://www.osdever.net/bkerndev/Docs/pit.htm)
	//uint32_t divisor = 1193180 / freq;
	//
	////El valor debe entrar en un entero de 16 bits
	//fail_if(divisor > (uint16_t)-1);
    //
    //outb(0x43, 0x36);	           /* Set our command byte 0x36 */
    //outb(0x40, divisor & 0xFF);   /* Set low byte of divisor */
    //outb(0x40, divisor >> 8);     /* Set high byte of divisor */	

	//..init data structure
	uint64_t i = 0;
	while(i<MAX_INSTANCES){
		sleep_pool[i] = 0;
		i++;
	}
	irq_sti();
}

uint64_t getFreeInstance(){
	uint64_t i = 0;
	while(i<MAX_INSTANCES){
		if(sleep_pool[i] == 0){//si esta inicializada en 0 => esta libre
			return i;
		}
		i++;
	}
	return MAX_INSTANCES;
}

//devuelve false si no se pudo instanciar una espera
bool sleep(uint64_t ticksCount){
	//simulamos "lock" con cli y sti
	irq_cli();
	uint64_t freeInstance = getFreeInstance();
	if(freeInstance == MAX_INSTANCES){//fallar si no hay instancias libres
		scrn_printf("No hay instancias libres para un sleep, intente nuevamente\n");
		irq_sti();
		return false;
	}


	//segun http://www.osdever.net/bkerndev/Docs/pit.htm
	//The default freq is 18.222Hz tick rate and was used in order
	//for the tick count to cycle at 0.055 seconds.
	//Using a 16-bit timer tick counter, the counter will overflow 
	//and wrap around to 0 once every hour.

	sleep_pool[freeInstance] = ticksCount;

	irq_sti();

	while(sleep_pool[freeInstance]>0);//sleep until irq clock calls finish

	return true;
}

void timer_tick(){
	//simulamos "lock" con cli y sti
	irq_cli();
	uint64_t i = 0;
	while(i<MAX_INSTANCES){
		if(sleep_pool[i]>0){
			sleep_pool[i]--;
			//scrn_printf("[Poll #%d]pending ticks: %d\n", i, sleep_pool[i]);
		}
		i++;
	}
	irq_sti();
}

