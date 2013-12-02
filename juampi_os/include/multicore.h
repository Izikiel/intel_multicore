#ifndef __MULTICORE_H
#define __MULTICORE_H

//TODO: Pasar a castellano, no se que carajo estaba pensando.

//Symmetric Multiprocessor Support (SMP)
//See: http://pdos.csail.mit.edu/6.828/2009/readings/ia32/MPspec.pdf

#include <types.h>

//MultiProcessor Configuration Table Entry. Table 4.3 of spec.
typedef enum { 
	PROCESSOR = 0, BUS, IOAPIC, IOINTR, LOCAL_IOINTR
} entry_type;

//MultiProcessor Configuration Table Entry for Processor. Table 4.4 of spec
typedef struct {
	//ID for Local APIC of processor
	uchar local_apic_id;
	//Version register number
	uchar version;	
	//Enabled bit. Zero if unusable
	uchar enabled : 1;
	//Bootstrap processor. One if it is
	uchar bootstrap : 1;
	//Reserved bits
	uchar __reserved : 6;
	//CPU Signature 
	//	Contains in consecutive order stepping (4 bits), model (4),	family (4)
	//	All following bits are reserved.
	//
	//	Table 4.5 has values.
	uint signature;
	//Feature flags as returned by CPUID instruction. Table 4.6 has values.
	uint features;
} __attribute__((__packed__)) processor_entry;

//MultiProcessor Configuration Table Entry for Bus. Table 4.7 of spec.
typedef struct {
	//Identifier. BIOS assigns identifiers starting from zero.
	uchar id;	
	//String identifying the type of bus;
	//Table 4.8 has possible values.
	char type[6];
} __attribute__((__packed__)) bus_entry;

//MultiProcessor Configuration Table Entry for IOAPIC. Table 4.9 of Spec
typedef struct {
	uchar id;
	//Version Register of APIC
	uchar version : 7;
	//Enabled bit. If zero the APIC is unusable
	uchar enabled : 1;
	//Base address of APIC
	void * base_address;
} __attribute__((__packed__)) ioapic_entry;

//MultiProcessor Configuration Table Entry for IO Interrupts. 
//	Table 4.10 of Spec
typedef struct {
	//Type of interrupt. Values in table 4-11
	uchar type;
	//Polarity of input signals. Table 4-10 has values.
	uchar polarity : 2;
	//Trigger mode of input signals. Table 4-10 has values.
	uchar trigger_mode : 2;
	//Identifies the bus from where this interrupt comes from.
	uchar source_bus_id;	
	//Identifies the interrupt from bus. Starts at 0
	uchar source_bus_irq;
	//Destination IO APIC Id
	uchar dest_apic_id;
	//Destination IO APIC INTIn 
	uchar dest_apic_intin;
} __attribute__((__packed__)) intr_assign_entry;

//MultiProcessor Configuration Table Entry for Local IO Interrupts. 
//	Table 4.12 of Spec
typedef struct {
	//Type of interrupt. Values in table 4-11
	uchar type;
	//Polarity of input signals. Table 4-10 has values.
	uchar polarity : 2;
	//Trigger mode of input signals. Table 4-10 has values.
	uchar trigger_mode : 2;
	//Identifies the bus from where this interrupt comes from.
	uchar source_bus_id;	
	//Identifies the interrupt from bus. Starts at 0
	uchar source_bus_irq;
	//Destination IO APIC Id
	uchar dest_apic_id;
	//Destination IO APIC LINTIn 
	uchar dest_apic_lintin;
} __attribute__((__packed__)) local_intr_assign_entry;

// MultiProcessor Configuration Table Entry in General.
typedef struct {
	//Entry type is a byte. Enums are ints by default in C. So no enum here.
	uchar entry_type;
	union {
		processor_entry			processor;	
		ioapic_entry			ioapic;
		bus_entry				bus;
		intr_assign_entry		intr_assign;
		local_intr_assign_entry	lintr_assign;
	};
} __attribute__((__packed__)) mp_entry;

//MultiProcessor Configuration Table. Table 4.2 of spec.
typedef struct mp_config_table{
	//Signature: Should be PCMP
	char signature[4];
	//Length of the base configuration in bytes, including header.
	ushort length;
	//Revision of spec.  1 for 1.1, 4 for 1.4
	uchar version;
	//Checksum of the base configuration table. 
	//All bytes including checksum bytes must equate zero.
	uchar checksum;
	//OEM ID: Name of manufacturer of system hardware. Not NULL Terminated.
	char oem_id[8];
	//Product ID: Name of product family of the system. Not NULL Terminated.
	char product_id[12];
	//OEM Table Pointer: Optional OEM defined config table. Zero if not defined.
	struct mp_config_table * oem_config_table;
	//OEM Table Size: Length of optional OEM table. Zero if not defined.
	ushort oem_table_length;
	//Entries following this base header in memory.
	ushort entry_count;
	//Base address by which each processor acceses the local apic
	void * local_apic_addr;
	//Length in bytes of the extended table entries. Zero if there are none.
	ushort extended_table_length;
	//Extended table checksum. All bytes of the extended table must sum to this
	//value. Zero if there are no extended entries.
	uchar extended_table_checksum;
	//Reserved space according to spec
	uchar __reserved;
	//Consecutive ENTRY_COUNT entries
	mp_entry entries[];
} __attribute__((__packed__)) mp_config_table;

//MultiProcessor Floating Point Structure. Table 4.1 of spec.
typedef struct {
	//Signature. Should be equal to __MP__
	char signature[4];
	//Configuration pointer: Contais information of the multiprocessor
	//configuration. All zeros if the configuration table does not exist.
	mp_config_table * config;
	//Length: Number of 16 byte chunks of this structure. Should be 1
	uchar length;
	//Version: Number of the MP Specification used. 1 for 1.1, 4 for 1.4
	uchar version;
	//Checksum: Sum of all the bytes in the struct should be cero.
	uchar checksum;
	//MP Features 1: Feature flags. When zero, indicates that a configuration
	//table is present. If not, indicates which default configuration the
	//system implements. See table 4-1 of the spec.
	uchar mp_features1;
	//MP Features 2: Bits 0-6 are reserved. Bit 7 indicates IMCR is present
	//and PIC Mode is implemented. Otherwise Virtual Wire Mode is implemented.
	uchar mp_features2;
	//MP Features 3: Reserved, must be zero.
	uchar mp_features3[3];
} __attribute__((__packed__)) mp_float_struct;

//Kernel entry point for multiprocessor inicialization
void multiprocessor_init();

#endif
