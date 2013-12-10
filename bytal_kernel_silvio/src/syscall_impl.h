/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  
*/
void impl_syscall80_handler(unsigned int eax, unsigned int ebx, unsigned int ecx);
void impl_syscall_fondear(unsigned int dirFisicaAMapear);
void impl_syscall_canonear(unsigned int dirFisicaTarget, unsigned int offsetRelativoContenidoMisil);
void impl_syscall_navegar(unsigned int dirFisicaPrimerPagina, unsigned int dirFisicaSegundaPagina);