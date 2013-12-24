#ifndef __COMMAND_SIGNATURES_H
#define __COMMAND_SIGNATURES_H

#include <types.h>

uint64_t command_paramtest(uint32_t argc, char argv[][101]);
uint64_t command_clear(uint32_t argc, char argv[][101]);
uint64_t command_sleep(uint32_t argc, char argv[][101]);

#endif