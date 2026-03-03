#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

char* platform_get_version(void);
char* platform_get_id(void);
uint32_t platform_get_boot_time(void);

#endif