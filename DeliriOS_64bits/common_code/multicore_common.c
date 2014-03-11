#include "multicore_common.h"

static void memset(void* dst, uint8_t value, uint32_t length){
	uint8_t* ptr = (uint8_t*) dst;
	for(uint32_t idx = 0; idx < length; idx++){
		ptr[idx] = value;
	}
}

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
void initialize_ipi_options(intr_command_register * options,
						delivery_mode_type delivery_mode,
						uint8_t vector,
						uint8_t destination)
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
void send_ipi(const intr_command_register * options)
{
	uint32_t * local_apic = (uint32_t *) DEFAULT_APIC_ADDR;
	//clear apic errors
	//local_apic[LAPIC_ERR_REG] = 0x0;
	const uint32_t * opts = (const uint32_t *) options;

	//Copiar opciones al mensaje que vamos a utilizar.
	//Se tiene que hacer de a 32 bits alineado a 16 bytes. Por eso el ICR
	//esta roto en dos pedazos.
	local_apic[LAPIC_ICR_DWORD1] = opts[1];
	//De acuerdo a Intel 3A 10.6.1, escribir la parte baja de este registro
	//hace que se envie la interrupcion. Lo escribimos la final por eso.
	local_apic[LAPIC_ICR_DWORD0] = opts[0];
}

void wait_for_ipi_reception(void)
{
	volatile uint32_t * local_apic = (volatile uint32_t *) DEFAULT_APIC_ADDR;
	//Esperar a la recepcion de la IPI.
	//El bit 12 es el bit de command completed. Cuando termine, nos vamos
	for(;local_apic[LAPIC_ICR_DWORD0] & (1 << 12););
}

void turn_on_apic_ap(){
	volatile uint32_t * local_apic = (volatile uint32_t *) DEFAULT_APIC_ADDR;
	local_apic[LAPIC_SPVEC_REG] |= (SPURIOUS_VEC_NUM << 4) & 0xF0;
    local_apic[LAPIC_SPVEC_REG] |= (1 << 8);
}