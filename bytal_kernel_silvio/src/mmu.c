/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Progra-mming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del manejador de memoria
*/

//map & unmap test
//mmu_unmapear_pagina(0x00008000, krnPageDir);//desmapeo la pagina 8
//mmu_mapear_pagina(0x00008000, krnPageDir, 0x00008000);//mapeo la pagina 8

#include "mmu.h"
#include "i386.h"
#include "utils.h"
#include "defines.h"
#include "contextManager.h"

//macro para armar entradas del directorio de paginas y de la tabla de paginas, como un constructor mas cavernicola style
#define PAGEDIR_ENTRY(pageDirTarget, numero, presentb, readWriteb, userSupervisorb, pageLevelWriteThroughb, pageLevelCacheDisableb, accesedb, ignoredb, pageSizeb, globalb, disponibleb, pageBased)                                                                                   \
    pageDirTarget[numero].present=presentb;        							\
    pageDirTarget[numero].readWrite=readWriteb;        						\
	pageDirTarget[numero].userSupervisor=userSupervisorb;        			\
    pageDirTarget[numero].pageLevelWriteThrough=pageLevelWriteThroughb;     \
    pageDirTarget[numero].pageLevelCacheDisable=pageLevelCacheDisableb;     \
    pageDirTarget[numero].accesed=accesedb;        							\
    pageDirTarget[numero].ignored=ignoredb;        							\
    pageDirTarget[numero].pageSize=pageSizeb;        						\
    pageDirTarget[numero].global=globalb;        							\
    pageDirTarget[numero].disponible=disponibleb;        					\
    pageDirTarget[numero].pageBase=pageBased;        							

#define PAGETABLE_ENTRY(pageTableTarget, numero, presentb, readWriteb, userSupervisorb, pageLevelWriteThroughb, pageLevelCacheDisableb, accesedb, dirtyBitb, disponibleb, pageBased)                                                                                   \
    pageTableTarget[numero].present=presentb;        						\
    pageTableTarget[numero].readWrite=readWriteb;        					\
	pageTableTarget[numero].userSupervisor=userSupervisorb;        			\
    pageTableTarget[numero].pageLevelWriteThrough=pageLevelWriteThroughb;   \
    pageTableTarget[numero].pageLevelCacheDisable=pageLevelCacheDisableb;   \
    pageTableTarget[numero].accesed=accesedb;        						\
    pageTableTarget[numero].dirtyBit=dirtyBitb;        						\
    pageTableTarget[numero].pageAttributeIndex=0;       					\
    pageTableTarget[numero].global=0;        							    \
    pageTableTarget[numero].disponible=disponibleb;        					\
    pageTableTarget[numero].pageBase=pageBased;        							
    
//declaro punteros(pensar como vectores) a la base de los arreglos de directorio de paginas y tabla de paginas
//asi tambien a la base de donde voy a mapear inicialmente las tareas en el mar
//y despues asigno a lo cavernicola las direcciones en donde tienen que comenzar
//notar que las definiciones extern necesarias de los punteros y las constantes estan en mmu.h
pagedir_entry* krnPageDir = (pagedir_entry*) KERNEL_PAGEDIR_POINTER;
pagetable_entry* krnFirstPageTable = (pagetable_entry*) KERNEL_FIRST_PAGETABLE_POINTER;
pagetable_entry* krnSecondPageTable = (pagetable_entry*) KERNEL_SECOND_PAGETABLE_POINTER;

//esta funcion inicializa el directorio de paginas y las 2 tablas de paginas necesarias que se piden por enunciado en el ej 3b.
//para el ejercicio 3b del tp me piden mapear los primeros 7.5mb entre 0x00000000 y 0x0077FFFF, necesito 2 entradas en el directorio de paginas, 
//la primera de 1024 entradas de 4k => 4mb y la segunda con 896 entradas de 4k => 3584 = 3.5mb
//las primeras 1024 paginas las puedo clavar en la tabla de paginas del kernel en 0x28000 segun el mapa de memoria
//las proximas 896 las voy a poner momentaneamente a partir de KERNEL_SECOND_PAGETABLE_POINTER por lo consultado a los ayudantes que realizaron el mapa de memoria
void mmu_inicializar_dir_kernel() {
	//marco todas como no presentes en el directorio de paginas, son 1024 entradas
	pageDirectoryInitialize(krnPageDir, 0, 0, 1023);
	//inicializo 1024 paginas con direcciones entre 0 y 1023*4096 marcandolas como presentes
	pageTableInitialize(krnFirstPageTable, 1, 0, 1023, 0);
	//marco las 1024 paginas como no presentes en la segunda tabla de paginas
	pageTableInitialize(krnSecondPageTable, 0, 0, 1023, 1024/*comenzar a mapear desde aqui*/);
	//inicializo las primeras 896 paginas para llenar 3.5mb como presentes
	pageTableInitialize(krnSecondPageTable, 1, 0, 895, 1024/*comenzar a mapear desde 1024 inclusive*/);
	
	//activo las primeras 2 paginas del directorio y les seteo las direcciones de las 2 tablas de paginas
	PAGEDIR_ENTRY(krnPageDir, 0/*index*/, 1/*present*/, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, 0, 0, (((unsigned int)(krnFirstPageTable))  >> 12) );
	PAGEDIR_ENTRY(krnPageDir, 1/*index*/, 1/*present*/, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, 0, 0, (((unsigned int)(krnSecondPageTable))  >> 12) );	
    //invalido la cache de traduccion de direcciones
    tlbflush();    
}

//inicializa el directorio de paginas referenciado por pageDirRef, marcando las tablas apuntadas por sus entradas
//en el rango [start..end] con los atributos acceso supervisor y lectura escritura, el atributo present se pasa por parametro
void pageDirectoryInitialize(pagedir_entry* pageDirRef, unsigned int present, unsigned int start, unsigned int end){
	while(start<=end){
		PAGEDIR_ENTRY(pageDirRef, start, present, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, 0, 0, 0);
		start++;
	}
}

//inicializa la tabla paginas referenciada por pageTableRef, marcando las paginas apuntadas por sus entradas
//en el rango [start..end] con los atributos acceso supervisor y lectura escritura, el atributo present se pasa por parametro,
//las paginas se mapean contiguamente a las posiciones fisicas segun indice el calculo mapOffset+start
void pageTableInitialize(pagetable_entry* pageTableRef, unsigned int present, unsigned int start, unsigned int end, unsigned int mapOffset){
	while(start<=end){
		PAGETABLE_ENTRY(pageTableRef, start, present, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, mapOffset+start/*no multiplico por 4096 pq son multiplos de paginas de 4k*/);
		start++;
	}
}

//Pre: todas las entradas del dir de paginas y de todas las tablas de paginas que se acceden estan previamente inicializadas
//es decir, cuando accedo a las pageBase los punteros son validos y los bits tienen algun valor inicializado a conciencia
//&& dir fisica esta alineada a 4k
//Post: pisa la pagina si existia anteriormente con el mapeo
void mmu_mapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase, unsigned int fisica, unsigned char readWriteb, unsigned char userSupervisorb){
    //obtengo los indices del dir de paginas y las tablas
    unsigned int pdIndex = (unsigned int)(virtual >> 22);//me quedo con los bits 31 a 22
    unsigned int ptIndex = (unsigned int)((virtual >> 12) & 0x03FF);//con la mascara me quedo unicamente con 10 bits de los bits 21 a 12    
    //obtengo el offset de la pagina
    unsigned int pageOffset = (unsigned int)(virtual & 0x0FFF);//me quedo con los primeros 12 bits

    //obtengo la tabla de paginas desde el directorio de paginas
    //shifteo 12 bits la direccion para completar el puntero de 32 bits a la tabla de paginas apuntada por la entrada del directorio
    
    //esto es un acceso valido por precondicion!
    pagetable_entry* pageTable = (pagetable_entry*) (pageDirBase[pdIndex].pageBase << 12);
    //PARAMETROS MACRO: pageTableTarget, index, userSupervisorb, pageLevelWriteThroughb, pageLevelCacheDisableb, accesedb, dirtyBitb, disponibleb, pageBased
    PAGETABLE_ENTRY(pageTable, ptIndex, 1/*present*/, readWriteb/*read+write*/, userSupervisorb/*supervisor*/, 0/*pageLevelWriteThroughb*/, 0/*pageLevelCacheDisableb*/, 0/*accesedb*/, 0/*dirtyBitb*/, 0/*disponibleb*/, ((fisica + pageOffset) >> 12)/*direccion >> 12 pues es multiplo de 4k*/);
    //seteo como presente la entrada en el directorio de paginas(podria pasar que no hubiera ninguna pagina todavia y ahora la hay)
    pageDirBase[pdIndex].present = 1;
    tlbflush();
}

//Pre: existe la pagina a desmapear
//Post: se desmapea la pagina, queda no presente en la tabla de paginas correspondiente.
void mmu_unmapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase){
    //obtengo los indices del dir de paginas y las tablas
    unsigned int pdIndex = (unsigned int)(virtual >> 22);//me quedo con los bits 31 a 22
    unsigned int ptIndex = (unsigned int)((virtual >> 12) & 0x03FF);//con la mascara me quedo unicamente con 10 bits de los bits 21 a 12    
    
    //obtengo la tabla de paginas desde el directorio de paginas
    //shifteo 12 bits la direccion para completar el puntero de 32 bits a la tabla de paginas apuntada por la entrada del directorio
    pagetable_entry* pageTable = (pagetable_entry*) (pageDirBase[pdIndex].pageBase << 12);

    //indexo la pagina en cuestion de la tabla y la seteo como no presente
    pageTable[ptIndex].present=0;//la marco como no presente
    //si no quedaron mas paginas presentes en toda la tabla, seteo como no presente la entrada del directorio?
    tlbflush();
}

unsigned int mmu_virtual2physic(unsigned int virtual, pagedir_entry* pageDirBase){
    //obtengo los indices del dir de paginas y las tablas
    unsigned int pdIndex = (unsigned int)(virtual >> 22);//me quedo con los bits 31 a 22
    unsigned int ptIndex = (unsigned int)((virtual >> 12) & 0x03FF);//con la mascara me quedo unicamente con 10 bits de los bits 21 a 12    
    
    //obtengo la tabla de paginas desde el directorio de paginas
    //shifteo 12 bits la direccion para completar el puntero de 32 bits a la tabla de paginas apuntada por la entrada del directorio
    pagetable_entry* pageTable = (pagetable_entry*) (pageDirBase[pdIndex].pageBase << 12);

    return (pageTable[ptIndex].pageBase << 12);
    //indexo la pagina en cuestion de la tabla y 
}

//las pilas de nivel 0 las voy a poner todas a partir de TASK_START_OFFSET_LEVEL0_STACKS_POINTER. Notar que ninguna pisa las estructuras de paginacion a partir de TASK_START_OFFSET_PAGINATION_STRUCTS_POINTER
unsigned int mmu_get_task_stack_level0(unsigned int taskNumber){
    return (TASK_START_OFFSET_LEVEL0_STACKS_POINTER + (0x2000 * taskNumber));//base + cada 2 paginas de 4k;
}

unsigned int mmu_get_flag_stack_level0(unsigned int taskNumber){
    return (TASK_START_OFFSET_LEVEL0_STACKS_POINTER + (0x2000 * taskNumber + 0x1000));//base + cada 2 paginas de 4k + corrimiento 1 pagina
}

pagedir_entry* mmu_get_task_pageDirAddress(unsigned int taskNumber){
    pagedir_entry* result;
    switch(taskNumber){
        case 8://la idle va mapeada sobre la CR3 del kernel ( es decir, su directorio de paginas )
            result = krnPageDir;
            break;
        default:
            result = (pagedir_entry*) (TASK_START_OFFSET_PAGINATION_STRUCTS_POINTER + (0x2000 * taskNumber));//base + cada 2 paginas de 4k estan las estructuras
            break;
    }
    return result;
}

pagetable_entry* mmu_get_task_pageTableAddress(unsigned int taskNumber){
    return (pagetable_entry*) (TASK_START_OFFSET_PAGINATION_STRUCTS_POINTER + (0x2000 * taskNumber) + 0x1000);//base + cada 2 paginas de 4k estan las estructuras
}

unsigned int mmu_get_task_physical_destination_task_page(unsigned int taskNumber){
    unsigned int result;
    switch(taskNumber){
        case 8:
            result = 0x20000;//tarea idle va mapeada sobre kernel para evitar que la ataquen en el mar, se romperia el scheduler y el maestruli se reiria.
            break;
        default:
            result = AREA_MAR_INICIO + (0x2000 * taskNumber);//direccion donde voy a mapear las tareas, (1gb)
            break;
    }
    return result;
}
// voy a copiar el codigo de las tareas a la direccion inicial del mar a partir del primer mega,
// esto en memoria fisica es a partir de la posicion 0x100000 (AREA_MAR_INICIO)
// voy a recibir por parametro el numero de tarea entre 0 y 7. voy a usar este parametro taskNumber para realizar
// el siguiente desplazamiento para las estructuras de paginas: TASK_START_OFFSET_PAGINATION_STRUCTS_POINTER + (8k * taskNumber). Notar que 8k son 2 paginas de 4k,
// a partir de la direccion base se guardan: Dir.Paginas, Tabla.Paginas, para cada una de las 8 tareas => 8k*8 = 64k
// Notar que debo mapear las siguientes paginas a cada tarea:
//  * Area de tierra:   las mismas 2 tablas de paginas que al kernel, con identity mapping, uso singleton y asigno a todas las tareas y al kernel, las mismas referencias
//                      a las tablas
//  * Paginas de codigo: 2 paginas de 4k , donde va a ir mapeado a la dir fisica en el mar que corresponda a la tarea. Las dir virtuales son 0x40000000, 0x40001000
//  * Pagina de ancla: una pagina de 4k, donde se va a mapear, inicialmente a la direccion fisica 0x0, correspondiente a tierra.

void mmu_inicializar_dir_tarea(unsigned int taskNumber){
    //calculo direccion del dir. de tablas y tabla de paginas de la tarea
    pagedir_entry* taskPageDir = mmu_get_task_pageDirAddress(taskNumber);
    pagetable_entry* taskPageTable = mmu_get_task_pageTableAddress(taskNumber);
    unsigned int taskPhysicDst = mmu_get_task_physical_destination_task_page(taskNumber);
    void* taskCodeSrc = (void*) (TASK_START_CODE_SRC_ADDR + (0x2000 * taskNumber));//base + cada 2 paginas de 4k esta el codigo

    //limpio memoria del dir de paginas SI NO ES LA IDLE, QUE USA CR3 DEL KERNEL!
    if(taskNumber!=8){
        pageDirectoryInitialize(taskPageDir, 0/*no presente*/, 0, 1023);
    }
    //limpio memoria de la tabla de paginas de la tarea
    pageTableInitialize(taskPageTable, 0/*no presente*/, 0, 1023, 0);

    //inicializo las paginas con identity mapping a tierra.
    //tengo las referencias de las 2 tablas que use para el kernel ya inicializadas con los permisos correspondientes
    //activo las primeras 2 paginas del directorio y les seteo las direcciones de las 2 tablas de paginas
    //notar que los indices son correctos 0 y 1 pues es sector tierra de memoria
    PAGEDIR_ENTRY(taskPageDir, 0/*index*/, 1/*present*/, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, 0, 0, (((unsigned int)(krnFirstPageTable)) >> 12) );
    PAGEDIR_ENTRY(taskPageDir, 1/*index*/, 1/*present*/, 1/*read+write*/, 0/*supervisor*/, 0, 0, 0, 0, 0, 0, 0, (((unsigned int)(krnSecondPageTable)) >> 12) );   

    //ahora debo mapear 3 paginas para las direcciones virtuales 0x40000000, 0x40001000, 0x40002000
    //las tareas van a estar mapeadas a las direcciones virtuales:
    // - 0x40000000: pagina 1 -> fisica inicialmente arriba del primer mega (sobre el mar), esto es a partir de 0x100000 dir fisica
    // - 0x40001000: pagina 2 -> fisica inicialmente arriba del primer mega (sobre el mar), esto es a partir de 0x100000 dir fisica contigua a la anterior
    // - 0x40002000: pagina ancla -> fisica 0x0 inicialmente

    //las funciones mapear y desmapear requieren como precondicion que toda la estructura del dir y tabla de paginas accedidas sea valida
    //como sabemos las direcciones estaticas virtuales, podemos inferir estos datos de ahi e inicializar manualmente las estructuras anteriormente

    //inicializo la tabla de paginas sobre el directorio, notar que el indice es siempre 0001 0000 0000b => (256dec) pues los primeros 10 bits de la dir virtual son iguales (0x400...)
    PAGEDIR_ENTRY(taskPageDir, 256/*index*/, 1/*present*/, 1/*read+write*/, 1/*usuario*/, 0, 0, 0, 0, 0, 0, 0, (((unsigned int)(taskPageTable))  >> 12) );

    mmu_remap_task_page(taskNumber, 1, taskPhysicDst + 0x0000);
    mmu_remap_task_page(taskNumber, 2, taskPhysicDst + 0x1000);
    mmu_remap_task_page(taskNumber, 3, 0x0/*ancla comienza en 0x0*/);

    //copia del codigo de la tarea a la direccion fisica correspondiente
    memcpy((void*)taskPhysicDst, taskCodeSrc, 8192/*8k = 2 paginolas piolas*/);
}

void mmu_remap_task_page(unsigned int taskNumber, unsigned int taskPageNumber, unsigned int newPhysicalAddress){
    pagedir_entry* taskPageDir =  mmu_get_task_pageDirAddress(taskNumber);
    switch(taskPageNumber){
        case 1:
            mmu_mapear_pagina(TASK_VIRTUAL_P1/*0x40000000*/, taskPageDir, newPhysicalAddress, 1/*readWriteb*/, 1/*userSupervisorb, modo USUARIO*/);
            break;
        case 2:
            mmu_mapear_pagina(TASK_VIRTUAL_P2/*0x40001000*/, taskPageDir, newPhysicalAddress, 1/*readWriteb*/, 1/*userSupervisorb, modo USUARIO*/);
            break;
        case 3:
            //calculo direccion del dir. de tablas y tabla de paginas de la tarea        
            mmu_mapear_pagina(TASK_VIRTUAL_P3/*0x40002000*/, taskPageDir, newPhysicalAddress, 0/*SOLO READ*/, 1/*userSupervisorb, modo USUARIO*/);
            break;
    }

    //notifico a context manager
    //actualizar informacion en base tareas(no me importa la idle)
    if(taskNumber<8){//ignoro la idle(task 8 contando desde 0)
        notificarCambioPagina(taskPageNumber/*numeroPaginaTarea*/,  newPhysicalAddress, taskNumber);
    }
}

//inicializa para las tareas [0..CANT_TAREAS] sus estructuras de paginacion y sus mapeos(cerrado el intervalo pq la task idle tambien cuenta)
void mmu_inicializar_dir_tareas(){
    unsigned int i=0;
    for(i=0;i<CANT_TAREAS+1/*la idle task tambien cuenta y es la novena contando desde 0*/;i++){
        mmu_inicializar_dir_tarea(i); 
    }

//    changePaginationContext((pagedir_entry*)mmu_get_task_pageDirAddress(0));
//    ejercicio 4c testeado sobre la tarea idle
//    unsigned int oldCR3 = changePaginationContext((pagedir_entry*)mmu_get_task_pageDirAddress(8));
//
//    unsigned char _modoEscritura = (0x4 << 4) | (0xF);
//    unsigned char _caracter = 176;//░
//
//    *((unsigned int*)0x66000) = (_modoEscritura << 8) | _caracter;
//    memcpy((void*)VIDEO, (void*)0x66000, 2/*copio 2 bytes*/);
//
//    changePaginationContext((pagedir_entry*)oldCR3);
}

//cambia el contexto del directorio de paginas, invalida la cache de direcciones y retorna el contenido viejo de CR3
unsigned int changePaginationContext(pagedir_entry* newDirContext){
    //usando estos "getters" y "setters" del registro del cpu CR3 voy a cambiar el contexto de memoria virtual
    //void lcr3(unsigned int val); => escribe a CR3
    //unsigned int rcr3(void); => lee el contenido de CR3
    unsigned int oldCR3 = rcr3();
    lcr3((unsigned int) newDirContext);

    //invalido la cache de traduccion de direcciones
    tlbflush();
    return oldCR3;
}
