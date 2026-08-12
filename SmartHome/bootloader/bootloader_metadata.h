#ifndef EDGEOTA_BOOTLOADER_METADATA_H
#define EDGEOTA_BOOTLOADER_METADATA_H

#include <stdint.h>

typedef enum {
    IMG_INVALID = 0,
    IMG_VALID = 1,
    IMG_PENDING = 2,
    IMG_CONFIRMED = 3,
    IMG_ROLLBACK = 4
} image_state_t;

enum {
    BL_SLOT_A = 0U,
    BL_SLOT_B = 1U,
    BL_SLOT_NONE = 0xFFU
};

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t meta_seq;

    uint8_t active_slot;
    uint8_t pending_slot;
    uint8_t confirmed_slot;
    uint8_t boot_attempts;

    uint32_t app_a_version;
    uint32_t app_a_size;
    uint32_t app_a_crc32;
    uint32_t app_a_state;

    uint32_t app_b_version;
    uint32_t app_b_size;
    uint32_t app_b_crc32;
    uint32_t app_b_state;

    uint32_t rollback_count;
    uint32_t min_allowed_version;
    uint32_t last_error;

    uint32_t metadata_crc32;
} ota_metadata_t;

uint32_t bl_crc32(const uint8_t* data, uint32_t len);
int bl_metadata_validate(const ota_metadata_t* meta);
void bl_metadata_default(ota_metadata_t* out);
void bl_metadata_update_crc(ota_metadata_t* meta);
void bl_metadata_load_or_default(ota_metadata_t* out);
int bl_metadata_any_valid(void);
int bl_metadata_commit(const ota_metadata_t* meta);
void bl_metadata_set_error(ota_metadata_t* meta, uint32_t err);
uint32_t bl_metadata_min_allowed_version(const ota_metadata_t* meta);

/* Validation failure reason for diagnostics */
enum {
    BL_META_OK = 0U,
    BL_META_ERR_NULL = 1U,
    BL_META_ERR_MAGIC = 2U,
    BL_META_ERR_SCHEMA = 3U,
    BL_META_ERR_CRC = 4U
};
extern volatile uint32_t g_bl_meta_validate_reason;

#endif
