#ifndef __TIEMPO_H__
#define __TIEMPO_H__

#define MEDIR_TIEMPO_START(start)							\
{															\
	unsigned int start_high, start_low;						\
	/* warn up ... */										\
	__asm__ __volatile__ (									\
		"cpuid\n\t"											\
		"rdtsc\n\t"											\
		"mov %%edx, %0\n\t"									\
		"mov %%eax, %1\n\t"									\
		: "=r" (start_high), "=r" (start_low)               \
		: /* no input */                                    \
		: "%eax"                                            \
	);                                                      \
	                                                        \
	__asm__ __volatile__ (									\
		"cpuid\n\t"                                         \
		"rdtsc\n\t"                                         \
		"mov %%edx, %0\n\t"                                 \
		"mov %%eax, %1\n\t"                                 \
		: "=r" (start_high), "=r" (start_low)               \
		: /* no input */									\
		: "%eax"                                            \
	);                                                      \
	                                                        \
	__asm__ __volatile__ (                                  \
		"cpuid\n\t"                                         \
		"rdtsc\n\t"											\
		"mov %%edx, %0\n\t"                                 \
		"mov %%eax, %1\n\t"                                 \
		: "=r" (start_high), "=r" (start_low)               \
		: /* no input */                                    \
		: "%eax"                                            \
	);														\
															\
	start = (((unsigned long long int) start_high) << 32) | \
		(unsigned long long int) (start_low);				\
}

#define MEDIR_TIEMPO_STOP(end)								\
{															\
	unsigned int end_high, end_low;							\
															\
	__asm__ __volatile__ (									\
		"cpuid\n\t"                                         \
		"rdtsc\n\t"                                         \
		"mov %%edx, %0\n\t"                                 \
		"mov %%eax, %1\n\t"                                 \
		: "=r" (end_high), "=r" (end_low)                   \
		: /* no input */                                    \
		: "%eax"											\
	);                                                      \
				                                            \
	end = (((unsigned long long int) end_high) << 32) | 	\
		(unsigned long long int) (end_low);					\
}

#endif /* !__TIEMPO_H__ */
