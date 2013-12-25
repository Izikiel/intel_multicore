#include <contextManager.h>
#include <keyboard.h>
#include <console.h>
#include <timer.h>

void notificarTecla(uint8_t keyCode)
{
    keyboard_handler(keyCode);
}

void notificarRelojTick()
{
	timer_tick();
	console_print_next_cursor();
}
