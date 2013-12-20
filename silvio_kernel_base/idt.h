#ifndef __IDT_H__
#define __IDT_H__

#include <stdint.h>

// Campo de atts de la IDT:
// |    	| Descriptor   |           |       |           | 
// |Present	| Privilege	   | 0 D 1 1 0 | 0 0 0 | 0 0 0 0 0 |
// |   		| Level		   |           |       |           |

// Trap
//attrs:
//P=1 present segment
//DPL=00 kernel level
//D=1 32 bits
//1000 1111 0000 0000 => 0x8F00
// | 1 | 0 0 | 0 1 1 1 1 | 0 0 0 | 0 0 0 0 0 | = 0x8F00

//Kernel
// Interrupt
//attrs:
//P=1 present segment
//DPL=00 user level
//D=1 32 bits
//1000 1110 0000 0000 => 0x8E00
// | 1 | 0 0 | 0 1 1 1 0 | 0 0 0 | 0 0 0 0 0 | = 0x8E00

//User
// Interrupt
//attrs:
//P=1 present segment
//DPL=11 user level
//D=1 32 bits
//1110 1110 0000 0000 => 0xEE00
// | 1 | 1 1 | 0 1 1 1 0 | 0 0 0 | 0 0 0 0 0 | = 0xEE00

#define IDT_ENTRIES_COUNT 255

#define KERNEL_TRAP_GATE_TYPE 0x8F00
#define KERNEL_INT_GATE_TYPE 0x8E00
#define SERVICE_INT_GATE_TYPE 0xEE00

/* Struct de descriptor de IDT */
typedef struct str_idt_descriptor {
    uint16_t  idt_length;
    uint64_t    idt_addr;
} __attribute__((__packed__)) idt_descriptor;

/* Struct de una entrada de la IDT */
typedef struct str_idt_entry_fld {
    uint16_t offset_0_15;
    uint16_t segsel;
    uint16_t attr;
    uint16_t offset_16_31;
    uint32_t offset_32_63;
    uint32_t zero;
} __attribute__((__packed__, aligned (8))) idt_entry;

extern idt_entry idt[];
extern idt_descriptor IDT_DESC;

void idt_inicializar();

#endif  /* !__IDT_H__ */
