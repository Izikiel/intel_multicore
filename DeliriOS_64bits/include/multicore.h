#ifndef __MULTICORE_H
#define __MULTICORE_H

//TODO: Pasar a castellano, no se que carajo estaba pensando.

//Symmetric Multiprocessor Support (SMP)
//See: http://pdos.csail.mit.edu/6.828/2009/readings/ia32/MPspec.pdf

#include <types.h>

//Pagina de codigo donde esta el codigo que van a ejecutar los cores ap
extern uint64_t ap_startup_code_page;

#define MP_ENTRY_TYPES 5

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

//MultiProcessor Configuration Table Entry. Table 4.3 of spec.
typedef enum {
	PROCESSOR = 0, BUS = 1, IOAPIC = 2, IOINTR = 3, LOCAL_IOINTR = 4
} entry_type;

//MultiProcessor Configuration Table Entry for Processor. Table 4.4 of spec
typedef struct {
	//ID for Local APIC of processor
	uint8_t local_apic_id;
	//Version register number
	uint8_t version;
	//Enabled bit. Zero if unusable
	uint8_t enabled : 1;
	//Bootstrap processor. One if it is
	uint8_t bootstrap : 1;
	//Reserved bits
	uint8_t __reserved : 6;
	//CPU Signature
	//	Contains in consecutive order stepping (4 bits), model (4),	family (4)
	//	All following bits are reserved.
	//
	//	Table 4.5 has values.
	uint8_t stepping : 4;
	uint8_t model : 4;
	uint8_t family : 4;
	//Reserved bits of CPU signature flag
	uint8_t __reserved_signature_high_nibble : 4;
	uint8_t __reserved_signature_byte : 4;
	uint16_t __reserved_signature_word;
	//Feature flags as returned by CPUID instruction. Table 4.6 has values.
	uint64_t features;
} __attribute__((__packed__)) processor_entry;

//MultiProcessor Configuration Table Entry for Bus. Table 4.7 of spec.
typedef struct {
	//Identifier. BIOS assigns identifiers starting from zero.
	uint8_t id;
	//String identifying the type of bus;
	//Table 4.8 has possible values.
	char type[6];
} __attribute__((__packed__)) bus_entry;

//MultiProcessor Configuration Table Entry for IOAPIC. Table 4.9 of Spec
typedef struct {
	uint8_t id;
	//Version Register of APIC
	uint8_t version : 7;
	//Enabled bit. If zero the APIC is unusable
	uint8_t enabled : 1;
	//Base address of APIC
	void * base_address;
} __attribute__((__packed__)) ioapic_entry;

//MultiProcessor Configuration Table Entry for IO Interrupts.
//	Table 4.10 of Spec
typedef struct {
	//Type of interrupt. Values in table 4-11
	uint8_t type;
	//Polarity of input signals. Table 4-10 has values.
	uint8_t polarity : 2;
	//Trigger mode of input signals. Table 4-10 has values.
	uint8_t trigger_mode : 2;
	//Identifies the bus from where this interrupt comes from.
	uint8_t source_bus_id;
	//Identifies the interrupt from bus. Starts at 0
	uint8_t source_bus_irq;
	//Destination IO APIC Id
	uint8_t dest_apic_id;
	//Destination IO APIC INTIn
	uint8_t dest_apic_intin;
} __attribute__((__packed__)) intr_assign_entry;

//MultiProcessor Configuration Table Entry for Local IO Interrupts.
//	Table 4.12 of Spec
typedef struct {
	//Type of interrupt. Values in table 4-11
	uint8_t type;
	//Polarity of input signals. Table 4-10 has values.
	uint8_t polarity : 2;
	//Trigger mode of input signals. Table 4-10 has values.
	uint8_t trigger_mode : 2;
	//Identifies the bus from where this interrupt comes from.
	uint8_t source_bus_id;
	//Identifies the interrupt from bus. Starts at 0
	uint8_t source_bus_irq;
	//Destination IO APIC Id
	uint8_t dest_apic_id;
	//Destination IO APIC LINTIn
	uint8_t dest_apic_lintin;
} __attribute__((__packed__)) local_intr_assign_entry;

// MultiProcessor Configuration Table Entry in General.
typedef struct {
	//Entry type is a byte. Enums are ints by default in C. So no enum here.
	uint8_t entry_type;
	union {
		processor_entry			processor;
		ioapic_entry			ioapic;
		bus_entry				bus;
		intr_assign_entry		intr_assign;
		local_intr_assign_entry	lintr_assign;
	} chunk;
} __attribute__((__packed__)) mp_entry;

//MultiProcessor Configuration Table. Table 4.2 of spec.
typedef struct mp_config_table{
	//Signature: Should be PCMP
	char signature[4];
	//Length of the base configuration in bytes, including header.
	uint16_t length;
	//Revision of spec.  1 for 1.1, 4 for 1.4
	uint8_t version;
	//Checksum of the base configuration table.
	//All bytes including checksum bytes must equate zero.
	uint8_t checksum;
	//OEM ID: Name of manufacturer of system hardware. Not NULL Terminated.
	char oem_id[8];
	//Product ID: Name of product family of the system. Not NULL Terminated.
	char product_id[12];
	//OEM Table Pointer: Optional OEM defined config table. Zero if not defined.
	struct mp_config_table * oem_config_table;
	//OEM Table Size: Length of optional OEM table. Zero if not defined.
	uint16_t oem_table_length;
	//Entries following this base header in memory.
	uint16_t entry_count;
	//Base address by which each processor acceses the local apic. 
	uint64_t local_apic_addr;
	//Length in bytes of the extended table entries. Zero if there are none.
	uint16_t extended_table_length;
	//Extended table checksum. All bytes of the extended table must sum to this
	//value. Zero if there are no extended entries.
	uint8_t extended_table_checksum;
	//Reserved space according to spec
	uint8_t __reserved;
	//Consecutive ENTRY_COUNT entries
	mp_entry entries[];
} __attribute__((__packed__)) mp_config_table;

//MultiProcessor Floating Point Structure. Table 4.1 of spec.
typedef struct {
	//Signature. Should be equal to _MP_
	char signature[4];
	//Configuration pointer: Contais information of the multiprocessor
	//configuration. All zeros if the configuration table does not exist.
	mp_config_table * config;
	//Length: Number of 16 byte chunks of this structure. Should be 1
	uint8_t length;
	//Version: Number of the MP Specification used. 1 for 1.1, 4 for 1.4
	uint8_t version;
	//Checksum: Sum of all the bytes in the struct should be cero.
	uint8_t checksum;
	//MP Features 1: Feature flags. When zero, indicates that a configuration
	//table is present. If not, indicates which default configuration the
	//system implements. See table 4-1 of the spec.
	uint8_t mp_features1;
	//MP Features 2: Bits 0-6 are reserved. Bit 7 indicates IMCR is present
	//and PIC Mode is implemented. Otherwise Virtual Wire Mode is implemented.
	uint8_t mp_features2;
	//MP Features 3: Reserved, must be zero.
	uint8_t mp_features3[3];
} __attribute__((__packed__)) mp_float_struct;

//IMCRP bit: Determines if Interrupt Mode Configuration Register is present.
//If so, the processor must initialize the IMCR to APIC Mode.
#define IMCRP_BIT (1 << 7)

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

//Kernel entry point for multiprocessor inicialization
void multiprocessor_init(void);

#endif
