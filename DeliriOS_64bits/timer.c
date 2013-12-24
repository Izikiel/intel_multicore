#include <timer.h>
#include <screen.h>
#include <irq.h>

static volatile uint64_t sleep_pool[MAX_INSTANCES];

void initialize_timer(uint64_t tickSpeed){
	//simulamos "lock" con cli y sti
	irq_cli();
	//initialize speed
	//...

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

