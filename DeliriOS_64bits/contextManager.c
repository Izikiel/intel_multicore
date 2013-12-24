#include <contextManager.h>
#include <keyboard.h>
#include <screen.h>

void notificarTecla(uint8_t keyCode)
{
    keyboard_handler(keyCode);
}

void notificarRelojTick()
{
	scrn_print_next_cursor();
}
