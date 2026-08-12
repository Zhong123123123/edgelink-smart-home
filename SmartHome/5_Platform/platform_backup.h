#ifndef __PLATFORM_BACKUP_H
#define __PLATFORM_BACKUP_H

#include <stdint.h>

typedef enum
{
	BOOT_REASON_UNKNOWN = 0,
	BOOT_REASON_POWER_ON,
	BOOT_REASON_PIN_RESET,
	BOOT_REASON_IWDG,
	BOOT_REASON_WWDG,
	BOOT_REASON_SOFTWARE,
	BOOT_REASON_BOR
} BootReason;

enum
{
	BKP_IDX_BOOT_COUNT = 1,
	BKP_IDX_LAST_RESET = 2,
	BKP_IDX_FAULT_PENDING = 3,
	BKP_IDX_LAST_FAULT_CODE = 4,
	BKP_IDX_LAST_DEAD_TASK = 5,
	BKP_IDX_SW_RESET_SRC = 6,
	BKP_IDX_FAULT_PC = 7,
	BKP_IDX_FAULT_LR = 8,
	BKP_IDX_FAULT_CFSR = 9,
	BKP_IDX_FAULT_HFSR = 10
};

typedef enum
{
	SW_RESET_SRC_UNKNOWN = 0,
	SW_RESET_SRC_GATEWAY_CMD = 1,
	SW_RESET_SRC_HARDFAULT = 2,
	SW_RESET_SRC_WATCHDOG = 3
} SwResetSource;

void PlatformBackup_Init(void);
uint32_t PlatformBackup_Read(uint32_t idx);
void PlatformBackup_Write(uint32_t idx, uint32_t value);
BootReason PlatformBackup_DetectAndClearResetReason(void);
const char *PlatformBackup_ResetReasonToString(BootReason reason);
const char *PlatformBackup_SwResetSourceToString(SwResetSource src);

#endif
