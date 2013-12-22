#include <contextManager.h>
#include <screen.h>

void notificarTecla(uint8_t keyCode)
{
	printInteger(keyCode, 0x07, 0, 6);
}

void notificarRelojTick()
{

}

void notificarExcepcion(uint32_t errorCode, uint64_t FLAGS, uint64_t RDI,
	uint64_t RSI, uint64_t RBP, uint64_t RSP, uint64_t RBX,
	 uint64_t RDX, uint64_t RCX, uint64_t RAX, uint64_t RIP)
{

}
