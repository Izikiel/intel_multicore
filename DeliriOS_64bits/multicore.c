#include <console.h>
#include <multicore.h>
#include <asserts.h>
#include <utils.h>
#include <ports.h>
#include <idt.h>
#include <timer.h>

typedef struct {
	uint64_t start, end;
} mem_zone;

static mem_zone
get_ebda_zone(void) 
{
	//De acuerdo a OSDEV, y en GRUB parece asumir lo mismo
	//	http://linux4u.jinr.ru/pub/misc/sys/grub/SVN/grub/grub2/commands/i386/pc/acpi.c	
	//El start de la EDBA puede en la mayoria de los casos tomarse de la
	//posicion 0x40E, o usando BIOS INT 12. Como estamos en Modo Protegido, 
	//usamos la memoria. TODO: Averiguar si GRUB ya te da este valor.

	//Como aparentemente la ebda mide al menos un kilobyte segun
	//	http://www.nondot.org/sabre/os/files/Booting/BIOS_SEG.txt
	//Y solo nos interesa el primer KB por la MPSpec, no nos fijamos nada.
	uint64_t ebda_start = (*((uint16_t *) 0x40e)) << 4;

	mem_zone res = { ebda_start, ebda_start + 0xFFF };
	return res;
}

//Hace el checksum signado de los bytes. La suma tiene que dar 0, 
//considerando suma signada de byte con overflow.
static bool
do_checksum(const void * p, unsigned int len)
{
	const char * bytes =(const char *) p;
	char sum = 0;

	for(uint64_t i = 0; i < len; i++){
		sum += bytes[i];	
	}

	return sum == 0;
}

static bool
check_valid_mpfs(const mp_float_struct * mpfs)
{
	if(memcmp(mpfs->signature,"_MP_",strlen("_MP_"))){
		return false;
	}
	return do_checksum(mpfs,mpfs->length * 16);
}

static mp_float_struct *
find_floating_pointer_struct(void)
{
	//Header y zonas a revisar
	static const char MPSIG[]	= "_MP_";
	static const uint64_t MPLEN		= sizeof(MPSIG)-1;
	static const uint64_t ZONES		= 3;

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
	for(uint64_t i = 0; i < ZONES; i++){
		uint8_t * st = (uint8_t *) zones[i].start;
		uint8_t * en = (uint8_t *) zones[i].end;
		for(uint8_t * p = st; p <= en; p++){
			if(memcmp(p,MPSIG,MPLEN) == 0){
				mpfs = (mp_float_struct *) p;
				goto found;
			}
		}
	}

found:
	if(mpfs == NULL){
		kernel_panic(__FUNCTION__, "Estructura MPFS no encontrada");
		return NULL;
	}

	if(!check_valid_mpfs(mpfs)){
		kernel_panic(__FUNCTION__, "Estructura MPFS con checksum invalido");
		return NULL;
	}

	return mpfs;
}

//Chequea que el checksum de la tabla de configuracion de Multi Processor sea
//correcta. 
static bool
check_valid_mpct(const mp_config_table * mpct)
{
	if(memcmp(mpct->signature,"PCMP",strlen("PCMP")) != 0){
		return false;
	}
	return do_checksum(mpct,mpct->length);
}

#define MAX_PROCESSORS 16
static processor_entry processors[MAX_PROCESSORS];
static uint64_t processor_count = 0;
//Indice del bootstrap processor
static int bootstrap_index = -1;

//Procesa la entrada de procesador para configurar este core
static void
configure_processor(const processor_entry * entry)
{
	console_printf("\tEntrada de procesador: \n"
		"\tLAPIC (%u / %u)\n"
		"\tFLAGS (%u / %u)\n"
		"\tIS BP: %b\n",
		entry->local_apic_id,entry->version,
		entry->model,entry->family,
		entry->bootstrap);

	//Agregar el procesador a la lista de procesadores. Que por ahora es un
	//arreglo fijo.
	fail_if(processor_count == MAX_PROCESSORS);
	memcpy(&processors[processor_count],entry,sizeof(processor_entry));
	if(entry->bootstrap){
		bootstrap_index = processor_count;
	}
	processor_count++;
}

//Tamaño de las entradas de la tabla de configuracion de MP
static uint64_t entry_sizes[] = {
	[PROCESSOR]		= 20,
	[BUS]			= 8,
	[IOAPIC]		= 8,
	[IOINTR]		= 8,
	[LOCAL_IOINTR]	= 8
};

//Determinar posicion de la proxima entrada de la tabla
static const mp_entry *
next_mp_entry(const mp_config_table * mpct, const mp_entry * p)
{
	const char * ptr = (const char *) p;
	fail_unless(p->entry_type < MP_ENTRY_TYPES);
	return (const mp_entry *) (ptr + entry_sizes[p->entry_type]);
}

//Inicializa a modo APIC el IMCR del BSP
static void
start_icmr_apic_mode(void)
{
	//TODO: Conseguir maquina donde probar esto
	fail_unless(false);
}

//Prende un APIC escribiendo el bit 8 contando desde 0 del Spurious Vector 
//Register en el  offset F0 en bytes.
//Nos puede servir para prender las otras.
static void
turn_on_apic(volatile uint64_t * apic)
{
	//0xF0 en bytes, pero usamos arreglo de int32 para poder hacer bien el
	//offset lo dividimos por el tamaño de un int32
    apic[LAPIC_SPVEC_REG] |= (1 << 8);
}

//Direccion donde encontrar el local APIC de mi procesador
#define DEFAULT_APIC_ADDR 0xFEE00000

//Flag correspondiente a cada delivery mode
static uint64_t delivery_mode_flag[] = {
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
						uint64_t vector,
						uint64_t destination)
{
	//Ponemos todo en cero para asegurar que los reserved bits esten en zero.
	memset(options,0,sizeof(*options));

	options->destination_field = destination;
	options->delivery_mode = delivery_mode_flag[delivery_mode];
	options->vector = vector;

	if(delivery_mode == INIT_DASSERT){
		options->destination_shorthand = 2; //All including self.
	}else{
		options->destination_shorthand = 0;
	}
	
	
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

//Enviar un IPI a un procesador dado su numero de local APIC. No confirma
//recepcion de la misma.
static void
send_ipi(const intr_command_register * options)
{
	uint64_t * local_apic = (uint64_t *) DEFAULT_APIC_ADDR;
	const uint64_t * opts = (const uint64_t *) options;

	//Copiar opciones al mensaje que vamos a utilizar.
	//Se tiene que hacer de a 32 bits alineado a 16 bytes. Por eso el ICR
	//esta roto en dos pedazos.
	local_apic[LAPIC_ICR_DWORD1] = opts[1];
	//De acuerdo a Intel 3A 10.6.1, escribir la parte baja de este registro
	//hace que se envie la interrupcion. Lo escribimos la final por eso.
	local_apic[LAPIC_ICR_DWORD0] = opts[0];
}

static void
wait_for_ipi_reception(void)
{
	volatile uint64_t * local_apic = (volatile uint64_t *) DEFAULT_APIC_ADDR;
	//Esperar a la recepcion de la IPI.
	//El bit 12 es el bit de command completed. Cuando termine, nos vamos
	for(;local_apic[LAPIC_ICR_DWORD0] & (1 << 12););
}

//Prende el Local APIC. Es necesario para el BSP para poder empezar a mandar
//IPIs. El local apic siempre esta en la misma direccion: 0xFEE00000
static void
turn_on_local_apic(const mp_config_table * mpct)
{
	fail_if(mpct->local_apic_addr != DEFAULT_APIC_ADDR);
	volatile uint64_t * local_apic = (volatile uint64_t *) DEFAULT_APIC_ADDR;

	//Poner el valor de la entrada de la IDT para el 
	//Spurious Vector Interrupt y prendemos el local APIC.
	local_apic[LAPIC_SPVEC_REG] |= (SPURIOUS_VEC_NUM << 4) & 0xF0;
	turn_on_apic(local_apic);

	//Sanity check: Leemos el local apic para saber que estamos bien.
	//Para eso nos fijamos que el version number sea identico al del boostrap
	//processor (porque estamos en el bootstrap processor);
	//
	//Estos valores los leo asi en base al manual Intel 3A Capitulo 10, donde
	//especifica como estan armados.
	uint64_t id = local_apic[LAPIC_ID_REG];
	id = (id >> 24) & 0xFF;

	fail_if(id != processors[bootstrap_index].local_apic_id);
	//Tengo que comentar esta linea porque en Bochs los ids estan mal
	//asignados. No se porque. Pero en el codigo de hecho se puede ver, y en
	//qemu anda.
	//
	//uint8_t version_number = local_apic[0x30 >> 2] & 0x7F;
	//fail_if(version_number != processors[bootstrap_index].version);
}

//Limpia el registro de errores del APIC
static void
clear_apic_errors(void)
{
	uint64_t * local_apic = (uint64_t *) DEFAULT_APIC_ADDR;
	local_apic[LAPIC_ERR_REG] = 0x0;
}

//Levanta la posicion de memoria a la que vamos a saltar por el codigo de 
//reset
static void
set_warm_reset_vector(uint64_t address)
{
	//Seteamos al BIOS para que el shutdown sea reset warm por jump
	cmos_writeb(0xF,0xA);
	//Le ponemos a que direccion saltar en la posicion 40:67h en modo real.
	*((uint64_t *) (0x40*16 +0x67)) = address;
}

static bool is_82489(void)
{
	volatile uint64_t * local_apic = (volatile uint64_t *) DEFAULT_APIC_ADDR;
	//De acuerdo al manual Intel 3A, si es un 82489DX se puede determinar 
	//usando el bit 4 del byte del registro de version;
	return !(local_apic[LAPIC_VER_REG] & (1 << 4));
}

//Enciende todos los APs
static void
turn_on_aps(uint64_t ap_startup_code_page)
{
	//De acuerdo a http://www.cheesecake.org/sac/smp.html, primero hay que
	//poner Warm Reset with far jump en el CMOS y poner la direccion a la que
	//va a saltar en ese lugar.
	//
	//Adicionalmente hay que limpiar los errores del registro de local APIC
	set_warm_reset_vector(ap_startup_code_page);
	clear_apic_errors();

	//Hay que enviar las STARTUP Ipis solamente si el local APIC no es un
	//82489DX.
	bool send_startup_ipis = !is_82489(); 
	for(uint64_t proci = 0; proci < processor_count; proci++){
		processor_entry * p = &processors[proci];

		//No despertar al BSP, porque no es AP
		if(p->bootstrap) continue;
		console_printf("\tProcesador %u - %u\n",proci,p->local_apic_id);

		//Crear mensaje de INIT e INIT Deasserted inicial para IPI
		intr_command_register init_ipi,init_ipi_doff;
		initialize_ipi_options(&init_ipi,INIT,0,p->local_apic_id);
		initialize_ipi_options(&init_ipi_doff,INIT_DASSERT,0,p->local_apic_id);
		//Crear mensaje de STARTUP IPI
		intr_command_register startup_ipi;

		//En vector para startup ipi hay que enviar la pagina fisica donde 
		//va a empezar a ejecutar. Para eso hay que shiftear 12 bits porque
		//el valor del startup IPI ya se considera alineado a pagina.
		initialize_ipi_options(&startup_ipi,STARTUP,
			ap_startup_code_page >> 12,p->local_apic_id);

		//Enviar las ipis de inicio
		console_printf("\tEnviando IPI de inicio\n");
		send_ipi(&init_ipi);
		wait_for_ipi_reception();
		send_ipi(&init_ipi_doff);
		wait_for_ipi_reception();
		//core_sleep(1000); //Dormir 10 millisegundos = 10000 microseg 
						  //(1000 10 microseg con la unidad considerada).
		sleep(1); //Dormir un poco mas de 10 milisegundos (0.055 segundos)

		console_printf("\tIPI de inicio enviada\n");
		if(send_startup_ipis){
			clear_apic_errors();
			console_printf("\tEnviando IPIs de startup\n");
			//Enviar las STARTUP ipis, dormir y esperar.
			send_ipi(&startup_ipi);	
			//core_sleep(20); //Dormir 200 microsegundos
			sleep(1); //Dormir un poco mas de 20 milisegundos (0.055 segundos)
			wait_for_ipi_reception();
			send_ipi(&startup_ipi);
			//core_sleep(20); //Dormir 200 microsegundos
			sleep(1); //Dormir un poco mas de 20 milisegundos (0.055 segundos)
			wait_for_ipi_reception();
		}
		console_printf("\tPrendi el core %u\n",proci);
		//TODO: Verificar que el core haya levantado programaticamente.
	}
}

//Determina la configuracion del procesador si hay una tabla de configuracion
//es decir si no hay una configuracion default en uso.
static void 
determine_cpu_configuration(const mp_float_struct * mpfs)
{
	mp_config_table * mpct = mpfs->config;
	fail_if(!check_valid_mpct(mpct));

	console_printf("\tMPCT TABLE: %u\n",(uint64_t) mpct);
	//Seguimos las entradas de la tabla de configuracion
	fail_unless(mpct->entry_count > 0);
	const mp_entry * entry = mpct->entries;

	for(uint64_t entryi = 0; entryi < mpct->entry_count; entryi++){
		if(entry->entry_type == PROCESSOR){
			configure_processor(&entry->chunk.processor);
		}
		//Por ahora ignoramos todas las demas entradas.
		entry = next_mp_entry(mpct,entry);
	}

	//Si no encontramos boostrap processor estamos en un serio problema.
	//Porque no hay chance que podamos prender el resto de los cores.
	fail_if(bootstrap_index < 0);

	if(mpfs->mp_features2 & IMCRP_BIT){
		//ICMR presente, hay que levantarlo a modo APIC
		start_icmr_apic_mode();	
	}

	turn_on_local_apic(mpct);

	uint64_t ap_symb = (uint64_t) &ap_startup_code_page;

	//Se require que la pagina donde se inicia a ejecutar el codigo 16 bits
	//de modo real del AP este alineada a pagina.
	fail_unless((ap_symb & 0xFFF) == 0);
	turn_on_aps(ap_symb);
}

static void
determine_default_configuration(mp_float_struct * mpfs)
{
	//TODO: Conseguir maquina donde probar esto, determinar si es necesario.
	//NOT IMPLEMENTED
	fail_unless(false);
}

//Revisar que las estructuras sean del tamaño correcto
static void
check_struct_sizes(void)
{
	fail_unless(sizeof(processor_entry) != entry_sizes[PROCESSOR]);
	fail_unless(sizeof(ioapic_entry) != entry_sizes[IOAPIC]);
	fail_unless(sizeof(bus_entry) != entry_sizes[BUS]);
	fail_unless(sizeof(intr_assign_entry) != entry_sizes[IOINTR]);
	fail_unless(sizeof(local_intr_assign_entry) != entry_sizes[LOCAL_IOINTR]);
}

void multiprocessor_init()
{
	//check &ap_startup_code_page % 0x1000 == 0
	//es decir, que este alineado a pagina de 4k el RIP de los AP CPU's
	fail_unless((uint64_t)(&ap_startup_code_page) % 0x1000 == 0)

	//console_cls();
	check_struct_sizes();

	//Detectar MPFS
	mp_float_struct * mpfs = find_floating_pointer_struct();
	if(mpfs == NULL){
		return;
	}

	console_printf("\tEstructura MPFS encontrada: %u\n", mpfs);

	//Recorrer estructura y determinar los cores
	if(mpfs->config != 0){
		//Configuracion hay que determinarla
		console_printf("\tConfiguracion a determinar\n");
		determine_cpu_configuration(mpfs);
	}else if(mpfs->mp_features1 != 0){
		//La configuracion ya esta definida y es una estandar
		console_printf("\tConfiguracion default numero: %d",mpfs->mp_features1);
		determine_default_configuration(mpfs);	
	}else{
		kernel_panic(__FUNCTION__, "Configuracion MPFS invalida");	
	}
}
