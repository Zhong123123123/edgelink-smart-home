#ifndef EDGEOTA_BOOTLOADER_CONFIG_H
#define EDGEOTA_BOOTLOADER_CONFIG_H

#include <stdint.h>

#define BL_FLASH_BASE            0x08000000UL
#define BL_BOOTLOADER_BASE       0x08000000UL
#define BL_BOOTLOADER_SIZE       (64UL * 1024UL)
#define BL_METADATA_BASE         0x08010000UL
#define BL_METADATA_SIZE         (64UL * 1024UL)
#define BL_APP_A_BASE            0x08020000UL
#define BL_APP_A_SIZE            (384UL * 1024UL)
#define BL_APP_B_BASE            0x08080000UL
#define BL_APP_B_SIZE            (384UL * 1024UL)
#define BL_CRASHLOG_BASE         0x080E0000UL
#define BL_CRASHLOG_SIZE         (128UL * 1024UL)

#define BL_META_MAGIC            0x454F5441UL /* 'EOTA' */
#define BL_META_SCHEMA_VERSION   2U
#define BL_MAX_BOOT_ATTEMPTS     3U
#define BL_OTA_IDLE_TIMEOUT_MS   60000U
/* Build mode:
 * 1 = DEV (allow boot by valid vector for active/confirmed slot, bypass CRC check there)
 * 0 = PROD (strict metadata size+crc+vector checks)
 */
#ifndef BL_DEV_BYPASS_CRC
#define BL_DEV_BYPASS_CRC        0U
#endif

#if BL_DEV_BYPASS_CRC
#warning "BOOTLOADER DEV MODE ENABLED: CRC BYPASS ACTIVE FOR ACTIVE/CONFIRMED SLOT. DO NOT USE FOR PRODUCTION."
#endif

/* Boot log UART (default: USART1 TX on PA9, 115200 8N1) */
#define BL_LOG_UART_ENABLE       1U
#define BL_LOG_BAUDRATE          115200UL
#define BL_LOG_APB2_CLK_HZ       16000000UL

/* Debug force boot slot:
 * BL_SLOT_NONE(0xFF): disabled (default)
 * BL_SLOT_A(0): force jump APP_A when vector is valid    0xFFU
 * BL_SLOT_B(1): force jump APP_B when vector is valid
 */
#ifndef BL_DEBUG_FORCE_BOOT_SLOT
#define BL_DEBUG_FORCE_BOOT_SLOT 0xFFU
// #define BL_DEBUG_FORCE_BOOT_SLOT 1U
#endif

#endif
