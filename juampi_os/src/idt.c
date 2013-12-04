#include <idt.h>
#include <utils.h>
#include <gdt.h>
#include <asserts.h>

idt_entry idt[256];
idt_desc IDT_DESC = {.idt_base = (uint)& idt, .idt_limit = sizeof(idt)-1};

uint RESERVED[] = {SPURIOUS_INTR_NUM, -1};
#define RESERVEDENTR (sizeof(RESERVED)/sizeof(RESERVED[0]))

static void 
idt_load_desc_unsafe(uint i, uint offset, ushort sel, idt_entry_flags flags)
{
	idt[i].offset_l = (ushort)(offset & 0xFFFF);
	idt[i].offset_h = (ushort)((offset >> 16) & 0xFFFF);
	idt[i].selector = sel;
	idt[i].__padding__ = 0;
	idt[i].type = flags.type | (flags.d << 3);
	idt[i].dpl = flags.dpl;
	idt[i].p = 1;
}

void idt_load_desc(uint i, uint offset, ushort sel, idt_entry_flags flags)
{
	//No pasarse de largo de la idt
	fail_if(i >= 256);
	//Revisar que no estemos usando una entrada reservada de la IDT.
	for(uint k = 0; k < RESERVEDENTR; k++){
		fail_unless(i != RESERVED[k]);
	}

	idt_load_desc_unsafe(i,offset,sel,flags);
}
#define IDT_LOAD_DESC(i,o,s,...) \
        idt_load_desc(i,o,s,(idt_entry_flags) { \
            .dpl = 0, .d = 1, .type = IDT_INT_GATE, __VA_ARGS__ })

#define IDT_LOAD_DESC_UNSAFE(i,o,s,...) \
        idt_load_desc_unsafe(i,o,s,(idt_entry_flags) { \
            .dpl = 0, .d = 1, .type = IDT_INT_GATE, __VA_ARGS__ })

static uint excph_addresses[] = {
	(uint)& _isr0, (uint)& _isr1, (uint)& _isr2, (uint)& _isr3, (uint)& _isr4,
	(uint)& _isr5, (uint)& _isr6, (uint)& _isr7, (uint)& _isr8, (uint)& _isr9,
	(uint)& _isr10, (uint)& _isr11, (uint)& _isr12, (uint)& _isr13, (uint)& _isr14,
	(uint)& _isr15, (uint)& _isr16, (uint)& _isr17, (uint)& _isr18, (uint)& _isr19
};

void idt_init_exceptions()
{
	memset((uchar*)idt,0,sizeof(idt));
	for(uint i = 0; i < 19; i++) {
		IDT_LOAD_DESC(i,excph_addresses[i],CODE_SEGMENT_KERNEL);
	}
}

static uint inter_handlers[] = {
	(uint)& _irq0, (uint)& _irq1, (uint)& _irq2, (uint)& _irq3, (uint)& _irq4,
	(uint)& _irq5, (uint)& _irq6, (uint)& _irq7, (uint)& _irq8, (uint)& _irq9,
	(uint)& _irq10, (uint)& _irq11, (uint)& _irq12, (uint)& _irq13, (uint)& _irq14,
	(uint)& _irq15
};

void idt_init_interrupts()
{
	//Ahora van los handlers de interrupciones por PIC
	for(uint i = 0; i < 16; i++) {
		IDT_LOAD_DESC(32+i, inter_handlers[i], CODE_SEGMENT_KERNEL);
	}

	//Evitar interrupciones spurias usando el spurious intr number. Esto se
	//usa para el APIC en el vector.
	IDT_LOAD_DESC_UNSAFE(SPURIOUS_INTR_NUM, (uint)&_irq_ignore_handler,\
		CODE_SEGMENT_KERNEL);
}
