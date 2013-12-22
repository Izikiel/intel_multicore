#include <kmain64.h>
#include <screen.h>

void startKernel64(){
	//pongo 5 porque es la linea donde termina de escribir kernel.asm con las macros de asm
	setInitialPrintingLine(5);
	printLine("DeliriOS iniciado.", redOnBlack);
	printLine("--------------------------------------------------------------------------------", modoEscrituraTexto);
	printLine("", modoEscrituraTexto);
}