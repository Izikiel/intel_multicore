#ifndef __MMU_H__
#define __MMU_H__

extern void* krnPML4T;
extern void* kernelStackPtrBSP;
extern void* kernelStackPtrAP1;

void init_64gb_identity_mapping();

#endif	/* !__MMU_H__ */
