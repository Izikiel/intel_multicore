#include "mmu.h"
#include "i386.h"
#include "utils.h"
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

void* kernelStackPtr = (void*)KERNEL_STACK_PTR;

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

    notificarMapeoPagina(virtual, fisica);
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
    
    notificarDesmapeoPagina(virtual);
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

//cambia el contexto del directorio de paginas, invalida la cache de direcciones y retorna el contenido viejo de CR3
unsigned int changePaginationContext(pagedir_entry* newDirContext){
    //usando estos "getters" y "setters" del registro del cpu CR3 voy a cambiar el contexto de memoria virtual
    //void lcr3(unsigned int val); => escribe a CR3
    //unsigned int rcr3(void); => lee el contenido de CR3
    unsigned int oldCR3 = rcr3();
    lcr3((unsigned int) newDirContext);

    //invalido la cache de traduccion de direcciones
    tlbflush();

    notificarCambioContexto(newDirContext);
    return oldCR3;
}
