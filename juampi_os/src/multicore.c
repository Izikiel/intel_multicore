#include <scrn.h>
#include <exception.h>
#include <multicore.h>
#include <asserts.h>
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

//Hace el checksum signado de los bytes. La suma tiene que dar 0, 
//considerando suma signada de byte con overflow.
static bool
do_checksum(void * p, unsigned int len)
{
	char * bytes =(char *) p;
	char sum = 0;

	for(uint i = 0; i < len; i++){
		sum += bytes[i];	
	}

	return sum == 0;
}

static bool
check_valid_mpfs(mp_float_struct * mpfs)
{
	return do_checksum(mpfs,mpfs->length * 16);
}

static mp_float_struct *
find_floating_pointer_struct()
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

	if(!check_valid_mpfs(mpfs)){
		scrn_printf("Estructura MPFS con checksum invalido");
		return NULL;
	}

	return mpfs;
}

//Chequea que el checksum de la tabla de configuracion de Multi Processor sea
//correcta. 
static bool
check_valid_mpct(mp_config_table * mpct)
{
	return do_checksum(mpct,mpct->length);
}

#define MAX_PROCESSORS 16
static processor_entry processors[MAX_PROCESSORS];
static uint processor_count = 0;

//Procesa la entrada de procesador para configurar este core
static void
configure_processor(processor_entry * entry)
{
	scrn_printf("\tEntrada de procesador: \n"
		"\tLAPIC (%d)\n"
		"\tFLAGS (%u - %u)\n"
		"\tIS BP: %b\n",
		entry->local_apic_id,entry->model,entry->family,entry->bootstrap);

	//Agregar el procesador a la lista de procesadores. Que por ahora es un
	//arreglo fijo.
	fail_if(processor_count == MAX_PROCESSORS);
	memcpy(&processors[processor_count],entry,sizeof(processor_entry));
	processor_count++;
}

//Tamaño de las entradas de la tabla de configuracion de MP
static uint entry_sizes[] = {
	[PROCESSOR]		= 20,
	[BUS]			= 8,
	[IOAPIC]		= 8,
	[IOINTR]		= 8,
	[LOCAL_IOINTR]	= 8
};

//Determinar posicion de la proxima entrada de la tabla
static mp_entry *
next_mp_entry(mp_config_table * mpct, mp_entry * p)
{
	char * ptr = (char *) p;
	fail_unless(p->entry_type < MP_ENTRY_TYPES);
	return (mp_entry *) (ptr + entry_sizes[p->entry_type]);
}

//Inicializa a modo APIC el IMCR del BSP
static void
start_icmr_apic_mode()
{
	//TODO
	scrn_printf("\tInicializar APIC del IMCR\n");
}

//Prende un APIC escribiendo el bit 8 contando desde 0 del Spurious Vector 
//Register en el  offset F0 en bytes.
//Nos puede servir para prender las otras.
static void
turn_on_apic(uint * apic)
{
	//0xF0 en bytes, pero usamos arreglo de int32 para poder hacer bien el
	//offset lo dividimos por el tamaño de un int32
    apic[0xF0 >> 2] |= (1 << 8);
}

#define DEFAULT_APIC_ADDR 0xFEE00000

//Flag correspondiente a cada delivery mode
static uint delivery_mode_flag[] = {
	[FIXED]			= 0,
	[LOWEST]		= 1,
	[SMI]			= 2,
	[NMI]			= 4,
	[INIT]			= 5,
	[INIT_DASSERT]	= 5,
	[STARTUP]		= 6
};

//Inicializar IPI options
static void
initialize_ipi_options(	intr_command_register * options,
						delivery_mode_type delivery_mode,
						uint vector,
						uint destination)
{
	//Ponemos todo en cero para asegurar que los reserved bits esten en zero.
	memset(options,0,sizeof(*options));

	options->destination_field = destination;
	options->delivery_mode = delivery_mode_flag[delivery_mode];
	options->vector = vector;
	
	//Para distinguir INIT y INIT DeAssert se usan flags distintos en otros
	//lados. Para eso utilizamos un modo separado de envio.
	if(delivery_mode == INIT_DASSERT){
		options->level = 0;
		options->trigger_mode = 1;
	}else{
		options->level = 1;
		options->trigger_mode = 0;
	}
}


//Enviar un IPI a un procesador dado su numero de local APIC
/*static void
send_ipi(intr_command_register * options)
{
	uchar * local_apic = (uchar *) DEFAULT_APIC_ADDR;
	//Revisar que no le estemos mandando una IPI al BSP. 
	uchar my_local_apic_id = local_apic[0x20];
	if(options->destination_field == my_local_apic_id)
		return;

	//Copiar opciones al mensaje que vamos a utilizar.
	intr_command_register *icr = (intr_command_register *) &local_apic[0x310];
	memcpy(icr,options,sizeof(*options));

	//Esperar a la recepcion de la IPI
	//El bit 12 es el bit de command completed. Cuando termine, nos vamos
	for(uchar recv = icr->delivery_status; recv; recv = icr->delivery_status);
}*/


//Prende el Local APIC. Es necesario para el BSP para poder empezar a mandar
//IPIs. El local apic siempre esta en la misma direccion: 0xFEE00000
static void
turn_on_local_apic(const mp_config_table * mpct)
{
	fail_if(mpct->local_apic_addr != DEFAULT_APIC_ADDR);
	uint * local_apic = (uint *) DEFAULT_APIC_ADDR;
    turn_on_apic(local_apic);
	//Poner el valor de la entrada de la IDT para el 
	//Spurious Vector Interrupt.
	local_apic[0xF0 >> 2] |= (SPURIOUS_VEC_NUM << 4) & 0xF0;
}

//Enciende todos los APs
static void
turn_on_aps()
{
	//TODO: Terminar esto.
	for(uint proci = 0; proci < processor_count; proci++){
		processor_entry * p = &processors[proci];

		//No despertar al bootstrap porque no es AP
		if(p->bootstrap) return;

		//Crear mensaje de INIT IPI
		intr_command_register init_ipi;
		initialize_ipi_options(&init_ipi,INIT,0,p->local_apic_id);

		//Crear mensaje de STARTUP IPI
		intr_command_register startup_ipi;
		//En vector para startup ipi hay que enviar la pagina fisica donde 
		//va a empezar a ejecutar 
		initialize_ipi_options(&startup_ipi,STARTUP,
			0,p->local_apic_id);
	}
}

//Determina la configuracion del procesador si hay una tabla de configuracion
//es decir si no hay una configuracion default en uso.
static void 
determine_cpu_configuration(const mp_float_struct * mpfs)
{
	mp_config_table * mpct = mpfs->config;
	fail_if(!check_valid_mpct(mpct));

	//Seguimos las entradas de la tabla de configuracion
	fail_unless(mpct->entry_count > 0);
	mp_entry * entry = mpct->entries;

	for(uint entryi = 0; entryi < mpct->entry_count; entryi++){
		if(entry->entry_type == PROCESSOR){
			configure_processor(&entry->chunk.processor);
		}
		//Por ahora ignoramos todas las demas entradas.
		entry = next_mp_entry(mpct,entry);
	}

	if(mpfs->mp_features2 & IMCRP_BIT){
		//ICMR presente, hay que levantarlo a modo APIC
		start_icmr_apic_mode();	
	}

	turn_on_local_apic(mpct);
	turn_on_aps();
}

static void
determine_default_configuration(mp_float_struct * mpfs)
{
}

//Revisar que las estructuras sean del tamaño correcto
static void
check_struct_sizes()
{
	fail_unless(sizeof(processor_entry) != entry_sizes[PROCESSOR]);
	fail_unless(sizeof(ioapic_entry) != entry_sizes[IOAPIC]);
	fail_unless(sizeof(bus_entry) != entry_sizes[BUS]);
	fail_unless(sizeof(intr_assign_entry) != entry_sizes[IOINTR]);
	fail_unless(sizeof(local_intr_assign_entry) != entry_sizes[LOCAL_IOINTR]);
}

void multiprocessor_init()
{
	scrn_cls();
	check_struct_sizes();

	//Detectar MPFS
	mp_float_struct * mpfs = find_floating_pointer_struct();
	if(mpfs == NULL){
		return;
	}

	scrn_printf("\tEstructura MPFS encontrada: %u\n", mpfs);

	//Recorrer estructura y determinar los cores
	if(mpfs->config != 0){
		//Configuracion hay que determinarla
		scrn_printf("\tConfiguracion a determinar\n");
		determine_cpu_configuration(mpfs);
	}else if(mpfs->mp_features1 != 0){
		//La configuracion ya esta definida y es una estandar
		scrn_printf("\tConfiguracion default numero: %d",mpfs->mp_features1);
		determine_default_configuration(mpfs);	
	}else{
		kernel_panic("Configuracion MPFS invalida");	
	}
}
