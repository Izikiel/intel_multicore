#include <contextManager.h>
#include <keyboard.h>
#include <screen.h>
#include <timer.h>

void notificarTecla(uint8_t keyCode)
{
    keyboard_handler(keyCode);
}

void notificarRelojTick()
{
	timer_tick();
	scrn_print_next_cursor();
}
