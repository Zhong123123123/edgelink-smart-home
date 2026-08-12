#ifndef __PLATFORM_STORAGE_H
#define __PLATFORM_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

bool platform_storage_init(void);
uint32_t platform_storage_read_jedec_id(void);
uint32_t platform_storage_get_size_bytes(void);
bool platform_storage_read(uint32_t addr, uint8_t *buf, uint32_t len);
bool platform_storage_page_program(uint32_t addr, const uint8_t *buf, uint32_t len);
bool platform_storage_sector_erase(uint32_t addr);

#endif
