#include "bootloader_metadata.h"
#include "bootloader_config.h"

#include <string.h>
#include "stm32f4xx_hal.h"

volatile uint32_t g_bl_meta_validate_reason = BL_META_OK;

enum {
    BL_META_RECORD_SIZE = (uint32_t)sizeof(ota_metadata_t),
    BL_META_MAX_RECORDS = BL_METADATA_SIZE / (uint32_t)sizeof(ota_metadata_t)
};

static int bl_meta_seq_is_newer(uint16_t candidate, uint16_t current) {
    uint16_t delta = (uint16_t)(candidate - current);
    return (delta != 0U && delta < 0x8000U) ? 1 : 0;
}

static int bl_meta_slot_is_erased(uint32_t addr) {
    const uint8_t* p = (const uint8_t*)addr;
    uint32_t i;
    for (i = 0U; i < BL_META_RECORD_SIZE; ++i) {
        if (p[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

static int bl_meta_find_latest(uint32_t* latest_addr, ota_metadata_t* latest_meta) {
    uint32_t i;
    int found = 0;
    ota_metadata_t best;
    uint32_t best_addr = 0U;

    for (i = 0U; i < BL_META_MAX_RECORDS; ++i) {
        uint32_t addr = BL_METADATA_BASE + i * BL_META_RECORD_SIZE;
        const ota_metadata_t* rec = (const ota_metadata_t*)addr;
        if (bl_meta_slot_is_erased(addr)) {
            break;
        }
        if (!bl_metadata_validate(rec)) {
            continue;
        }
        if (!found || bl_meta_seq_is_newer(rec->meta_seq, best.meta_seq)) {
            best = *rec;
            best_addr = addr;
            found = 1;
        }
    }

    if (found) {
        if (latest_addr != 0) {
            *latest_addr = best_addr;
        }
        if (latest_meta != 0) {
            *latest_meta = best;
        }
    }
    return found;
}

static int bl_meta_find_append_addr(uint32_t* out_addr) {
    uint32_t i;
    if (out_addr == 0) {
        return 0;
    }
    for (i = 0U; i < BL_META_MAX_RECORDS; ++i) {
        uint32_t addr = BL_METADATA_BASE + i * BL_META_RECORD_SIZE;
        if (bl_meta_slot_is_erased(addr)) {
            *out_addr = addr;
            return 1;
        }
    }
    return 0;
}

static int bl_metadata_write_record(uint32_t addr, const ota_metadata_t* meta) {
    uint32_t i;
    const uint8_t* src = (const uint8_t*)meta;
    const uint8_t* dst = (const uint8_t*)addr;

    for (i = 0U; i < BL_META_RECORD_SIZE; ++i) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + i, src[i]) != HAL_OK) {
            return 0;
        }
    }
    for (i = 0U; i < BL_META_RECORD_SIZE; ++i) {
        if (dst[i] != src[i]) {
            return 0;
        }
    }
    return bl_metadata_validate((const ota_metadata_t*)addr);
}

static int bl_metadata_erase_sector(void) {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0U;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_4;
    erase.NbSectors = 1;
    return (HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK) ? 1 : 0;
}

uint32_t bl_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint8_t b;
    for (i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (b = 0; b < 8U; ++b) {
            if ((crc & 1UL) != 0UL) {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            } else {
                crc >>= 1U;
            }
        }
    }
    return ~crc;
}

int bl_metadata_validate(const ota_metadata_t* meta) {
    ota_metadata_t tmp;
    uint32_t crc;
    g_bl_meta_validate_reason = BL_META_OK;
    if (meta == 0) {
        g_bl_meta_validate_reason = BL_META_ERR_NULL;
        return 0;
    }
    if (meta->magic != BL_META_MAGIC) {
        g_bl_meta_validate_reason = BL_META_ERR_MAGIC;
        return 0;
    }
    if (meta->schema_version != BL_META_SCHEMA_VERSION) {
        g_bl_meta_validate_reason = BL_META_ERR_SCHEMA;
        return 0;
    }

    tmp = *meta;
    tmp.metadata_crc32 = 0U;
    crc = bl_crc32((const uint8_t*)&tmp, (uint32_t)sizeof(tmp));
    if (crc != meta->metadata_crc32) {
        g_bl_meta_validate_reason = BL_META_ERR_CRC;
        return 0;
    }
    return 1;
}

void bl_metadata_default(ota_metadata_t* out) {
    if (out == 0) return;
    memset(out, 0, sizeof(*out));
    out->magic = BL_META_MAGIC;
    out->schema_version = BL_META_SCHEMA_VERSION;
    out->meta_seq = 1U;
    out->active_slot = BL_SLOT_A;
    out->pending_slot = BL_SLOT_NONE;
    out->confirmed_slot = BL_SLOT_A;
    out->boot_attempts = 0U;
    out->app_a_state = IMG_VALID;
    out->app_b_state = IMG_INVALID;
    out->min_allowed_version = 1U;
    bl_metadata_update_crc(out);
}

void bl_metadata_update_crc(ota_metadata_t* meta) {
    if (meta == 0) {
        return;
    }
    meta->metadata_crc32 = 0U;
    meta->metadata_crc32 = bl_crc32((const uint8_t*)meta, (uint32_t)sizeof(*meta));
}

void bl_metadata_load_or_default(ota_metadata_t* out) {
    if (out == 0) {
        return;
    }
    if (bl_meta_find_latest(0, out)) {
        return;
    }
    bl_metadata_default(out);
}

int bl_metadata_any_valid(void) {
    return bl_meta_find_latest(0, 0);
}

void bl_metadata_set_error(ota_metadata_t* meta, uint32_t err) {
    if (meta == 0) {
        return;
    }
    meta->last_error = err;
    bl_metadata_update_crc(meta);
}

int bl_metadata_commit(const ota_metadata_t* meta) {
    ota_metadata_t work;
    uint32_t append_addr = 0U;

    if (meta == 0) {
        return 0;
    }

    work = *meta;
    if (work.meta_seq == 0U) {
        work.meta_seq = 1U;
    }
    bl_metadata_update_crc(&work);

    HAL_FLASH_Unlock();

    if (bl_meta_find_append_addr(&append_addr)) {
        if (!bl_metadata_write_record(append_addr, &work)) {
            HAL_FLASH_Lock();
            return 0;
        }
        HAL_FLASH_Lock();
        return 1;
    }

    if (!bl_metadata_erase_sector()) {
        HAL_FLASH_Lock();
        return 0;
    }

    if (!bl_metadata_write_record(BL_METADATA_BASE, &work)) {
        HAL_FLASH_Lock();
        return 0;
    }

    HAL_FLASH_Lock();
    return 1;
}

uint32_t bl_metadata_min_allowed_version(const ota_metadata_t* meta) {
    if (meta == 0) {
        return 0U;
    }
    return (meta->min_allowed_version == 0U) ? 1U : meta->min_allowed_version;
}
