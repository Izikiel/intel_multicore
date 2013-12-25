#ifndef __COMMAND_SIGNATURES_H
#define __COMMAND_SIGNATURES_H

#include <types.h>

uint64_t command_listcommands(const uint32_t argc, const char argv[][101]);
uint64_t command_paramtest(const uint32_t argc, const char argv[][101]);
uint64_t command_clear(const uint32_t argc, const char argv[][101]);
uint64_t command_sleep(const uint32_t argc, const char argv[][101]);


#endif