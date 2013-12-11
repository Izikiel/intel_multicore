/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de estructuras para administrar tareas
*/

#include "tss.h"
#include "mmu.h"

#define DEFAULT_EFLAGS 0x00000202


#define TSS_ENTRY(ptl, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt, dtrap, iomap)	                                                               			\
  	 	(tss) {									\
		(unsigned short)  ptl,					\
	    (unsigned short)  0x0000/*unused0*/,	\
	    (unsigned int)    esp0,					\
	    (unsigned short)  ss0,					\
	    (unsigned short)  0x0000/*unused1*/,	\
	    (unsigned int)    esp1,					\
	    (unsigned short)  ss1,					\
	    (unsigned short)  0x0000/*unused2*/,	\
	    (unsigned int)    esp2,					\
	    (unsigned short)  ss2,					\
	    (unsigned short)  0x0000/*unused3*/,	\
	    (unsigned int)    cr3,					\
	    (unsigned int)    eip,					\
	    (unsigned int)    eflags,				\
	    (unsigned int)    eax,					\
	    (unsigned int)    ecx,					\
	    (unsigned int)    edx,					\
	    (unsigned int)    ebx,					\
	    (unsigned int)    esp,					\
	    (unsigned int)    ebp,					\
	    (unsigned int)    esi,					\
	    (unsigned int)    edi,					\
	    (unsigned short)  es,					\
	    (unsigned short)  0x0000/*unused4*/,	\
	    (unsigned short)  cs,					\
	    (unsigned short)  0x0000/*unused5*/,	\
	    (unsigned short)  ss,					\
	    (unsigned short)  0x0000/*unused6*/,	\
	    (unsigned short)  ds,					\
	    (unsigned short)  0x0000/*unused7*/,	\
	    (unsigned short)  fs,					\
	    (unsigned short)  0x0000/*unused8*/,	\
	    (unsigned short)  gs,					\
	    (unsigned short)  0x0000/*unused9*/,	\
	    (unsigned short)  ldt,					\
	    (unsigned short)  0x0000/*unused10*/,	\
	    (unsigned short)  dtrap,				\
	    (unsigned short)  iomap					\
	}	

#define TSS_TASK_DESC(taskNumber)																	\
							TASKS_TSS_DESC[taskNumber] = (gdt_descriptor) {							\
														    sizeof(tss_navios[taskNumber]) - 1,		\
														    (unsigned int) &tss_navios[taskNumber]	\
														};											\

#define TSS_FLAG_DESC(taskNumber)																	\
							FLAGS_TSS_DESC[taskNumber] = (gdt_descriptor) {							\
														    sizeof(tss_banderas[taskNumber]) - 1,	\
														    (unsigned int) &tss_banderas[taskNumber]\
														};


#define TSS_TASK_ENTRY(taskNumber)																	\
				tss_navios[taskNumber] = TSS_ENTRY(													\
							0x0000/*ptl*/, 															\
							mmu_get_task_stack_level0(taskNumber)/*esp0*/, 							\
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*ss0*/, 								\
							0x00000000/*esp1*/, 													\
							0x0000/*ss1*/, 															\
							0x00000000/*esp2*/, 													\
							0x0000/*ss2*/, 															\
							mmu_get_task_pageDirAddress(taskNumber)/*cr3*/, 						\
							TASK_CODE/*eip*/, 														\
							DEFAULT_EFLAGS/*eflags*/, 												\
							0x00000000/*eax*/, 														\
							0x00000000/*ecx*/, 														\
							0x00000000/*edx*/, 														\
							0x00000000/*ebx*/, 														\
							TASK_STACK_BASE/*esp*/, 												\
							TASK_STACK_BASE/*ebp*/, 												\
							0x00000000/*esi*/, 														\
							0x00000000/*edi*/, 														\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*es*/,		\
							( (GDT_IDX_SEGCODE_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*cs*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*ss*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*ds*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*fs*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*gs*/,		\
							0x0000/*ldt*/, 															\
							0x0000/*dtrap*/, 														\
							0xFFFF/*iomap*/);																		

#define TSS_FLAG_ENTRY(taskNumber)																	\
					tss_banderas[taskNumber] = TSS_ENTRY(											\
							0x0000/*ptl*/, 															\
							mmu_get_flag_stack_level0(taskNumber)/*esp0*/, 							\
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*ss0*/, 								\
							0x00000000/*esp1*/, 													\
							0x0000/*ss1*/, 															\
							0x00000000/*esp2*/, 													\
							0x0000/*ss2*/, 															\
							mmu_get_task_pageDirAddress(taskNumber)/*cr3*/, 						\
							dameFuncionBandera(taskNumber)/*eip*/,									\
							DEFAULT_EFLAGS/*eflags*/, 												\
							0x00000000/*eax*/, 														\
							0x00000000/*ecx*/, 														\
							0x00000000/*edx*/, 														\
							0x00000000/*ebx*/, 														\
							(TASK_CODE + 0x1FFC)/*esp*/, 												\
							(TASK_CODE + 0x1FFC)/*ebp*/,											\
							0x00000000/*esi*/, 														\
							0x00000000/*edi*/, 														\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*es*/,		\
							( (GDT_IDX_SEGCODE_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*cs*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*ss*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*ds*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*fs*/,		\
							( (GDT_IDX_SEGDATA_LEVEL3_DESC << 3) | 3/*pongo rpl en 3*/)/*gs*/,		\
							0x0000/*ldt*/, 															\
							0x0000/*dtrap*/, 														\
							0xFFFF/*iomap*/);	

tss tarea_inicial = TSS_ENTRY(0x0000/*ptl*/, 
							0x00000000/*esp0*/, 
							0x0000/*ss0*/, 
							0x00000000/*esp1*/, 
							0x0000/*ss1*/, 
							0x00000000/*esp2*/, 
							0x0000/*ss2*/, 
							0x00000000/*cr3*/, 
							0x00000000/*eip*/, 
							DEFAULT_EFLAGS/*eflags*/, 
							0x00000000/*eax*/, 
							0x00000000/*ecx*/, 
							0x00000000/*edx*/, 
							0x00000000/*ebx*/, 
							0x00000000/*esp*/, 					
							0x00000000/*ebp*/, 
							0x00000000/*esi*/, 
							0x00000000/*edi*/, 
							0x0000/*es*/, 
							0x0000/*cs*/, 
							0x0000/*ss*/, 
							0x0000/*ds*/, 
							0x0000/*fs*/, 
							0x0000/*gs*/, 
							0x0000/*ldt*/, 
							0x0000/*dtrap*/, 
							0x0000/*iomap*/);

tss tarea_idle = TSS_ENTRY( 0x0000/*ptl*/, 
							0x00000000/*esp0*/,
							0x0000/*ss0*/, 
							0x00000000/*esp1*/, 
							0x0000/*ss1*/, 
							0x00000000/*esp2*/, 
							0x0000/*ss2*/, 
							KERNEL_PAGEDIR_POINTER/*cr3 del kernel*/, 
							TASK_CODE/*eip*/, 
							DEFAULT_EFLAGS/*eflags*/, 
							0x00000000/*eax*/, 
							0x00000000/*ecx*/, 
							0x00000000/*edx*/, 
							0x00000000/*ebx*/, 
							IDLE_TASK_STACK0_BASE/*esp*/, 	//fijate que en 0x2A000 en expand down pisa la 2da tabla de paginas del kernel que esta en 0x29000. HELL YEAH, seria un lindo bug si no lo hubiera visto.
							IDLE_TASK_STACK0_BASE/*ebp*/, 	//fijate que en 0x2A000 en expand down pisa la 2da tabla de paginas del kernel que esta en 0x29000. HELL YEAH, seria un lindo bug si no lo hubiera visto.
							0x00000000/*esi*/, 
							0x00000000/*edi*/, 
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*es*/, //segmento de datos de kernel y rpl 0
							(GDT_IDX_SEGCODE_LEVEL0_DESC << 3)/*cs*/, //segmento de codigo de kernel y rpl 0
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*ss*/, //segmento de datos de kernel y rpl 0
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*ds*/, //segmento de datos de kernel y rpl 0
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*fs*/, //segmento de datos de kernel y rpl 0
							(GDT_IDX_SEGDATA_LEVEL0_DESC << 3)/*gs*/, //segmento de datos de kernel y rpl 0
							0x0000/*ldt*/, 
							0x0000/*dtrap*/, 
							0xFFFF/*iomap*/);  //mascara de acceso a dispositivos de E/S habilitadisima
	
tss tss_navios[CANT_TAREAS];
tss tss_banderas[CANT_TAREAS];

gdt_descriptor IDLE_TSS_DESC = {
    sizeof(tarea_idle) - 1,
    (unsigned int) &tarea_idle
};

gdt_descriptor INIT_TSS_DESC = {
    sizeof(tarea_inicial) - 1,
    (unsigned int) &tarea_inicial
};

gdt_descriptor TASKS_TSS_DESC[CANT_TAREAS];
gdt_descriptor FLAGS_TSS_DESC[CANT_TAREAS];														

void tss_inicializar() {
	//tareas entre 0 a 7 para inicializar
	int taskNumber=0;
	for(taskNumber=0;taskNumber<8;taskNumber++){
		TSS_TASK_ENTRY(taskNumber)
		TSS_TASK_DESC(taskNumber)
		TSS_FLAG_ENTRY(taskNumber)
		TSS_FLAG_DESC(taskNumber)
	}
}

void tss_reiniciar_esp_bandera(unsigned int flagNumber){
	tss_banderas[flagNumber].esp = (TASK_CODE + 0x1FFC);
	tss_banderas[flagNumber].eip = dameFuncionBandera(flagNumber);
}

unsigned int dameFuncionBandera(unsigned int taskNumber){
	unsigned int * punteroFuncionBandera = (unsigned int*) ((0x10000) + (0x2000 * taskNumber) + 0x1FFC);
	return (unsigned int) (*punteroFuncionBandera) + (unsigned int) 0x40000000;

	//TODO pasar 0x10000... a mmu_virtual2physic de la pagina1 que es el mismo codigo
	//sino si te bombardean la bandera y el codigo no se veria afectado porque se usa siempre el original de tierra
	//nice try... me tira clock on flag exception esto de abajo que deberia ser la solucion.
	//unsigned int* banderaPtrFisica = (unsigned int*) mmu_virtual2physic(FLAG_CODE, mmu_get_task_pageDirAddress(taskNumber));
	//return (*banderaPtrFisica) + 0x40000000;
}