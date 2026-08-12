#ifndef EDGEOTA_BOOTLOADER_FLASH_H
#define EDGEOTA_BOOTLOADER_FLASH_H

#include <stddef.h>
#include <stdint.h>

int bl_flash_erase_slot(uint32_t slot_base);
int bl_flash_write(uint32_t address, const uint8_t* data, size_t len);
int bl_flash_is_slot_base(uint32_t base);

#endif
