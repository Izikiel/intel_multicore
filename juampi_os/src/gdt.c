#include <types.h>
#include <utils.h>
#include <gdt.h>
#include <tss.h>
#include <exception.h>
#include <irq.h>

seg_desc gdt[GDT_COUNT];
gdt_desc GDT_DESC = {
	.gdt_base = (uint)& gdt,
	.gdt_limit = sizeof(gdt)-1
};

void gdt_load_desc(uint i, uint base, uint limit, seg_flags flags)
{
	gdt[i].base_l   = (ushort)(base & 0xFFFF);
	gdt[i].limit_l  = (ushort)(limit & 0xFFFF);
	gdt[i].base_m   = (uchar)((base & ~0xFFFF) >> 16);
	gdt[i].type = flags.type;
	gdt[i].s    = flags.s;
	gdt[i].g    = flags.g;
	gdt[i].dpl  = flags.dpl;
	gdt[i].avl  = flags.avl;//0;
	gdt[i].p    = flags.p;//1;
	gdt[i].db   = flags.db;//1;
	gdt[i].limit_h  = (uchar)((limit & ~0xFFFF) >> 16);
	gdt[i].base_h   = (uchar)((base & ~0xFFFFFF) >> 24);
}

void gdt_init()
{
	memset(gdt,0,sizeof(gdt));
	//Configuracion flat.
	GDT_LOAD_DESC(1,0,0xFFFFF, .type = CODE_ER_NC, .dpl = 0);
	GDT_LOAD_DESC(2,0,0xFFFFF, .type = DATA_RW, .dpl = 0);
	GDT_LOAD_DESC(3,0,0xFFFFF, .type = CODE_ER_NC, .dpl = 3);
	GDT_LOAD_DESC(4,0,0xFFFFF, .type = DATA_RW, .dpl = 3);
}

short gdt_add_tss(uint tss_virtual)
{
	uint eflags = irq_cli();
	uint i;
	short res = -1;
	for(i = 1; i < GDT_COUNT; i++) {
		if(!gdt[i].p) {
			GDT_LOAD_DESC(i,tss_virtual,sizeof(tss)-1,
			              .type = TSS_AVL, .g = 1, .s = 0,
			              .dpl = 0, .db = 0);
			break;
		}
	}
	if(i < GDT_COUNT) {
		res = i << 3;
	}
	irq_sti(eflags);
	return res;
}

void gdt_remove_tss(short tss_selector)
{
	if(!tss_selector) {
		return;
	}
	short index = tss_selector >> 3;
	if(gdt[index].type != TSS_AVL && gdt[index].type != TSS_BUSY)
		kernel_panic("Se trato de remover una entrada de GDT\n"
		             "que no es una entrada de TSS");
	memset(&gdt[index],0,sizeof(seg_desc));
}
