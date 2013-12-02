#include <scrn.h>
#include <multicore.h>
#include <utils.h>

typedef struct {
	uint start, end;
} mem_zone;

static mem_zone
get_ebda_zone() 
{
	//De acuerdo a OSDEV, y en GRUB parece asumir lo mismo
	//	http://linux4u.jinr.ru/pub/misc/sys/grub/SVN/grub/grub2/commands/i386/pc/acpi.c	
	//El start de la EDBA puede en la mayoria de los casos tomarse de la
	//posicion 0x40E, o usando BIOS INT 12. Como estamos en Modo Protegido, 
	//usamos la memoria. TODO: Averiguar si GRUB ya te da este valor.

	//Como aparentemente la ebda mide al menos un kilobyte segun
	//	http://www.nondot.org/sabre/os/files/Booting/BIOS_SEG.txt
	//Y solo nos interesa el primer KB por la MPSpec, no nos fijamos nada.
	uint ebda_start = (*((ushort *) 0x40e)) << 4;

	mem_zone res = { ebda_start, ebda_start + 0xFFF };
	return res;
}

static mp_float_struct *
check_valid_mpfs(mp_float_struct * mpfs)
{
	char * bytes =(char *) mpfs;
	//La suma de la espeficacion es con signo, considerando overflow.
	char sum = 0;

	//Length es en cachos de 16 bytes
	uint len = mpfs->length * 16;

	for(uint i = 0; i < len; i++){
		sum += bytes[i];	
	}
		
	if(sum != 0){
		scrn_printf("Checksum de MPFS invalido: %d\n",sum);
		return NULL;
	}
	return mpfs;
}

static mp_float_struct *
find_floating_point_struct()
{
	//Header y zonas a revisar
	static const char MPSIG[]	= "_MP_";
	static const uint MPLEN		= sizeof(MPSIG)-1;
	static const uint ZONES		= 3;

	//Direcciones son inclusive (intervalo cerrado).
	mem_zone zones[] = {
		//Primer kilobyte de Extended Data BIOS Area
		get_ebda_zone(),
		//El ultimo KB de memoria base. Asumimos que 640 KB es el minimo
		//ignorando que obviamente eso debiera alcanzarle a cualquiera.
		{ (639 << 10), (639 << 10) + 0xFFF },
		//Direcciones de BIOS ROM
		{ 0xF0000, 0xFFFFF }
	};

	//Buscar header en las zonas indicadas
	mp_float_struct * mpfs = NULL;
	for(uint i = 0; i < ZONES; i++){
		uchar * st = (uchar *) zones[i].start;
		uchar * en = (uchar *) zones[i].end;
		for(uchar * p = st; p <= en; p++){
			if(memcmp(p,MPSIG,MPLEN) == 0){
				mpfs = (mp_float_struct *) p;
				goto found;
			}
		}
	}

found:
	if(mpfs == NULL){
		scrn_printf("Estructura MPFS no encontrada");
		return NULL;
	}

	scrn_printf("Candidato: %u\n",mpfs);
	return check_valid_mpfs(mpfs);
}

void multiprocessor_init()
{
	mp_float_struct * mpfs = find_floating_point_struct();
	if(mpfs == NULL){
		return;
	}
}
