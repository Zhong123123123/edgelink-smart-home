#include "platform_backup.h"

#include "board_profile.h"

static volatile uint32_t *bkp_reg_ptr(uint32_t idx)
{
	if (idx > 19U)
	{
		return (volatile uint32_t *)&RTC->BKP0R;
	}
	return (&RTC->BKP0R) + idx;
}

void PlatformBackup_Init(void)
{
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();
	__HAL_RCC_RTC_ENABLE();
}

uint32_t PlatformBackup_Read(uint32_t idx)
{
	PlatformBackup_Init();
	return *bkp_reg_ptr(idx);
}

void PlatformBackup_Write(uint32_t idx, uint32_t value)
{
	PlatformBackup_Init();
	*bkp_reg_ptr(idx) = value;
}

BootReason PlatformBackup_DetectAndClearResetReason(void)
{
	uint32_t csr = RCC->CSR;
	BootReason reason = BOOT_REASON_UNKNOWN;

	if ((csr & RCC_CSR_IWDGRSTF) != 0U)
	{
		reason = BOOT_REASON_IWDG;
	}
	else if ((csr & RCC_CSR_WWDGRSTF) != 0U)
	{
		reason = BOOT_REASON_WWDG;
	}
	else if ((csr & RCC_CSR_SFTRSTF) != 0U)
	{
		reason = BOOT_REASON_SOFTWARE;
	}
	else if ((csr & RCC_CSR_PINRSTF) != 0U)
	{
		reason = BOOT_REASON_PIN_RESET;
	}
	else if ((csr & RCC_CSR_BORRSTF) != 0U)
	{
		reason = BOOT_REASON_BOR;
	}
	else if ((csr & RCC_CSR_PORRSTF) != 0U)
	{
		reason = BOOT_REASON_POWER_ON;
	}

	__HAL_RCC_CLEAR_RESET_FLAGS();
	return reason;
}

const char *PlatformBackup_ResetReasonToString(BootReason reason)
{
	switch (reason)
	{
		case BOOT_REASON_POWER_ON: return "POWER_ON";
		case BOOT_REASON_PIN_RESET: return "PIN_RESET";
		case BOOT_REASON_IWDG: return "IWDG";
		case BOOT_REASON_WWDG: return "WWDG";
		case BOOT_REASON_SOFTWARE: return "SOFTWARE";
		case BOOT_REASON_BOR: return "BOR";
		default: return "UNKNOWN";
	}
}

const char *PlatformBackup_SwResetSourceToString(SwResetSource src)
{
	switch (src)
	{
		case SW_RESET_SRC_GATEWAY_CMD: return "gateway_cmd";
		case SW_RESET_SRC_HARDFAULT: return "hardfault";
		case SW_RESET_SRC_WATCHDOG: return "watchdog";
		default: return "unknown";
	}
}
