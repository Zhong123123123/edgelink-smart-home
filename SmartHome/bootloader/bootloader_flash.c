#include "bootloader_flash.h"

#include "bootloader_config.h"
#include "stm32f4xx_hal.h"

static uint32_t bl_flash_pack_word(const uint8_t* data) {
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8U)
        | ((uint32_t)data[2] << 16U)
        | ((uint32_t)data[3] << 24U);
}

int bl_flash_is_slot_base(uint32_t base) {
    return (base == BL_APP_A_BASE || base == BL_APP_B_BASE) ? 1 : 0;
}

int bl_flash_erase_slot(uint32_t slot_base) {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0U;

    if (!bl_flash_is_slot_base(slot_base)) {
        return 0;
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.NbSectors = 3;
    erase.Sector = (slot_base == BL_APP_A_BASE) ? FLASH_SECTOR_5 : FLASH_SECTOR_8;

    HAL_FLASH_Unlock();
    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }
    HAL_FLASH_Lock();
    return 1;
}

int bl_flash_write(uint32_t address, const uint8_t* data, size_t len) {
    uint32_t cur_addr = address;
    const uint8_t* cur = data;
    size_t remain = len;
    const uint8_t* verify;

    if (data == 0 || len == 0U) {
        return 0;
    }

    HAL_FLASH_Unlock();

    while ((remain > 0U) && ((cur_addr & 0x3U) != 0U)) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, cur_addr, *cur) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        cur_addr++;
        cur++;
        remain--;
    }

    while (remain >= 4U) {
        uint32_t word = bl_flash_pack_word(cur);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, cur_addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        cur_addr += 4U;
        cur += 4;
        remain -= 4U;
    }

    while (remain > 0U) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, cur_addr, *cur) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        cur_addr++;
        cur++;
        remain--;
    }

    HAL_FLASH_Lock();

    verify = (const uint8_t*)address;
    cur = data;
    remain = len;
    while (remain > 0U) {
        if (*verify != *cur) {
            return 0;
        }
        verify++;
        cur++;
        remain--;
    }

    return 1;
}
