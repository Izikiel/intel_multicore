#include <types.h>
//Direccion donde encontrar el local APIC de mi procesador
#define DEFAULT_APIC_ADDR 0xFEE00000

//Ids of different Local Apic register positions (looking at the APIC as a 32
//bit array).
typedef enum{
	LAPIC_ID_REG		= 0x20 >> 2,
	LAPIC_VER_REG		= 0x30 >> 2,
	LAPIC_SPVEC_REG		= 0xF0 >> 2,
	LAPIC_ERR_REG		= 0x280 >> 2,
	LAPIC_ICR_DWORD0	= 0x300 >> 2,
	LAPIC_ICR_DWORD1	= 0x310 >> 2,
} local_apic_regs;

//Interrupt Command Register (ICR). See Section 10.6
typedef struct {
	//The vector number of the interrupt being sent.
	uint8_t vector;
	//Specifies the type of IPI to be sent.
	uint8_t delivery_mode:3;
	//Selects either physical (0) or logical (1) destination mode
	//(see Section 10.6.2, “Determining IPI Destination”).
	uint8_t destination_mode:1;
	// Indicates the IPI delivery status:
	//
	// 0(idle) indicates the local
	// apic has completed sending any previous IPIs.
	//
	// 1 (send pending) Indicates that this local apic has not completed
	// sending the last IPI.
	uint8_t delivery_status:1;
	uint8_t __reserved1:1;
	//For the INIT level de-assert delivery mode this flag must be set to 0;
	//for all other delivery modes it must be set to 1.
	uint8_t level:1;
	//Selects the trigger mode when using the INIT level de-assert delivery
	//mode: edge (0) or level (1)
	uint8_t trigger_mode:1;
	uint8_t __reserved2:2;
	// Shorthands are defined for the following cases:
	//	No shorthand(00) (Destination specified in destination field),
	//	Software self interrupt (01),
	//	IPIs to all processors in the system including the sender (10),
	//	IPIs to all processors in the system excluding the sender (11).
	uint8_t destination_shorthand:2;
	//Specifies the target processor or processors.
	//This field is only used when the destination shorthand field is set
	//to 00B.
	uint8_t __reserved3 :4;
	uint8_t __reserved4[4];
	// Destination field
	//	00: if the destination mode is physical, the destination field
	//		contains the APIC ID of the destination
	//
	//	when the mode is logical, the interpretation of this field can be
	//	found in Intel SDM Vol 3 Chapter 10
	uint8_t destination_field;
} __attribute__((__packed__)) intr_command_register;

typedef enum {
	FIXED = 0, LOWEST, SMI, NMI, INIT, INIT_DASSERT, STARTUP
} delivery_mode_type;


void initialize_ipi_options(intr_command_register * options,
						delivery_mode_type delivery_mode,
						uint8_t vector,
						uint8_t destination);

void send_ipi(const intr_command_register * options);

void wait_for_ipi_reception(void);
