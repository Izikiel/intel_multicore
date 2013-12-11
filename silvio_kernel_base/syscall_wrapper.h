#ifndef __SYSCALL_H__
#define __SYSCALL_H__

#define LS_INLINE static __inline __attribute__((always_inline))

#define SYS_FONDEAR     0x923
#define SYS_CANONEAR    0x83A
#define SYS_NAVEGAR     0xAEF

/*
 * Syscalls
 */
LS_INLINE unsigned int syscall_fondear(unsigned int dir) {
    int ret;

    __asm __volatile(
        "mov %0, %%eax \n"
        "mov %1, %%ebx \n"
        "int $0x50     \n"
        : /* no output*/
        : "r" (SYS_FONDEAR), "m" (dir)
        : "eax", "ebx"
    );

    __asm __volatile("mov %%eax, %0" : "=r" (ret));

    return ret;
}

LS_INLINE unsigned int syscall_canonear(unsigned int dir_usuario, unsigned int dir_relativa) {
    int ret;

    __asm __volatile(
        "mov %0, %%eax \n"
        "mov %1, %%ebx \n"
        "mov %2, %%ecx \n"
        "int $0x50     \n"
        : /* no output*/
        : "r" (SYS_CANONEAR), "m" (dir_usuario), "m" (dir_relativa)
        : "eax", "ebx", "ecx"
    );

    __asm __volatile("mov %%eax, %0" : "=r" (ret));

    return ret;
}

LS_INLINE unsigned int syscall_navegar(unsigned int dir_primera_pag, unsigned int dir_segunda_pag) {
    int ret;

    __asm __volatile(
        "mov %0, %%eax \n"
        "mov %1, %%ebx \n"
        "mov %2, %%ecx \n"
        "int $0x50     \n"
        : /* no output*/
        : "r" (SYS_NAVEGAR), "m" (dir_primera_pag), "m" (dir_segunda_pag)
        : "eax", "ebx", "ecx"
    );

    __asm __volatile("mov %%eax, %0" : "=r" (ret));

    return ret;
}

LS_INLINE unsigned int syscall_bandera_fin(unsigned int dir_bandera_buffer) {
    int ret;

    __asm __volatile(
        "mov %0, %%eax \n"
        "int $0x66     \n"
        : /* no output*/
        : "r" (dir_bandera_buffer)
        : "eax"
    );

    __asm __volatile("mov %%eax, %0" : "=r" (ret));

    return ret;
}

#endif  /* !__SYSCALL_H__ */
