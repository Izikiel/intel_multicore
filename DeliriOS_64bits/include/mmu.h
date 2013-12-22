#ifndef __MMU_H__
#define __MMU_H__

extern void* krnPML4T;
extern void* kernelStackPtr;

void init_64gb_identity_mapping();

#endif	/* !__MMU_H__ */
