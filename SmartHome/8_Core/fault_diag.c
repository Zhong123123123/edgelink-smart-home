#include "fault_diag.h"

#include <stdio.h>
#include <string.h>

#include "app_storage.h"
#include "app_watchdog.h"
#include "board_pins.h"
#include "config.h"
#include "log.h"
#include "platform_backup.h"

#define FAULT_MAGIC 0x464C5448UL

#if defined(__GNUC__) || defined(__clang__)
__attribute__((section(".noinit")))
#endif
static volatile FaultSnapshot g_fault_snapshot;

static uint32_t fault_crc32(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFUL;
	uint32_t i;
	uint8_t bit;
	for (i = 0U; i < len; ++i)
	{
		crc ^= (uint32_t)data[i];
		for (bit = 0U; bit < 8U; ++bit)
		{
			crc = ((crc & 1UL) != 0UL) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
		}
	}
	return ~crc;
}

/* Best-effort panic print on gateway UART (polling, no IRQ dependency). */
static void fault_uart_putc(char c)
{
	USART_TypeDef *u = BOARD_GW_UART_INSTANCE;
	uint32_t spin = 2000000UL;
	while ((((u->SR) & USART_SR_TXE) == 0U) && (spin-- > 0U))
	{
	}
	u->DR = (uint16_t)(uint8_t)c;
}

static void fault_uart_puts(const char *s)
{
	while (*s != '\0')
	{
		fault_uart_putc(*s++);
	}
}

static void fault_uart_hex32(uint32_t v)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;
	fault_uart_puts("0x");
	for (i = 7; i >= 0; --i)
	{
		fault_uart_putc(hex[(v >> ((uint32_t)i * 4U)) & 0x0FU]);
	}
}

void FaultDiag_HardFaultCapture(uint32_t *stack, uint32_t exc_lr)
{
	FaultSnapshot snap;
	uint8_t *dst;
	const uint8_t *src;
	uint32_t i;
	for (i = 0U; i < (uint32_t)sizeof(snap); ++i)
	{
		((uint8_t *)&snap)[i] = 0U;
	}
	snap.magic = FAULT_MAGIC;
	snap.stacked_r0 = stack[0];
	snap.stacked_r1 = stack[1];
	snap.stacked_r2 = stack[2];
	snap.stacked_r3 = stack[3];
	snap.stacked_r12 = stack[4];
	snap.stacked_lr = stack[5];
	snap.stacked_pc = stack[6];
	snap.stacked_xpsr = stack[7];
	snap.exc_lr = exc_lr;
	snap.cfsr = SCB->CFSR;
	snap.hfsr = SCB->HFSR;
	snap.dfsr = SCB->DFSR;
	snap.afsr = SCB->AFSR;
	snap.mmfar = SCB->MMFAR;
	snap.bfar = SCB->BFAR;
	snap.tick = 0U;
	snap.crc32 = 0U;
	snap.crc32 = fault_crc32((const uint8_t *)&snap, (uint32_t)sizeof(snap));

	fault_uart_puts("\r\n[FAULT][PANIC] hardfault pc=");
	fault_uart_hex32(snap.stacked_pc);
	fault_uart_puts(" lr=");
	fault_uart_hex32(snap.stacked_lr);
	fault_uart_puts(" cfsr=");
	fault_uart_hex32(snap.cfsr);
	fault_uart_puts(" hfsr=");
	fault_uart_hex32(snap.hfsr);
	fault_uart_puts("\r\n");

	dst = (uint8_t *)&g_fault_snapshot;
	src = (const uint8_t *)&snap;
	for (i = 0U; i < (uint32_t)sizeof(snap); ++i)
	{
		dst[i] = src[i];
	}

	PlatformBackup_Write(BKP_IDX_FAULT_PENDING, 1U);
	PlatformBackup_Write(BKP_IDX_LAST_FAULT_CODE, snap.cfsr);
	PlatformBackup_Write(BKP_IDX_FAULT_PC, snap.stacked_pc);
	PlatformBackup_Write(BKP_IDX_FAULT_LR, snap.stacked_lr);
	PlatformBackup_Write(BKP_IDX_FAULT_CFSR, snap.cfsr);
	PlatformBackup_Write(BKP_IDX_FAULT_HFSR, snap.hfsr);
	PlatformBackup_Write(BKP_IDX_SW_RESET_SRC, (uint32_t)SW_RESET_SRC_HARDFAULT);

	{
		USART_TypeDef *u = BOARD_GW_UART_INSTANCE;
		uint32_t spin = 2000000UL;
		while ((((u->SR) & USART_SR_TC) == 0U) && (spin-- > 0U))
		{
		}
	}

	NVIC_SystemReset();
}

void FaultDiag_InitAtBoot(void)
{
	BootReason reason;
	uint32_t boot_count;
	char msg[128];
	uint32_t dead_task;
	const char *dead_task_name = "unknown";
	reason = PlatformBackup_DetectAndClearResetReason();
	boot_count = PlatformBackup_Read(BKP_IDX_BOOT_COUNT) + 1U;
	PlatformBackup_Write(BKP_IDX_BOOT_COUNT, boot_count);
	PlatformBackup_Write(BKP_IDX_LAST_RESET, (uint32_t)reason);
	dead_task = PlatformBackup_Read(BKP_IDX_LAST_DEAD_TASK);
	switch (dead_task)
	{
		case WD_TASK_GATEWAY: dead_task_name = "gateway"; break;
		case WD_TASK_SENSOR: dead_task_name = "sensor"; break;
		case WD_TASK_ALARM: dead_task_name = "alarm"; break;
		case WD_TASK_STORAGE: dead_task_name = "storage"; break;
		case WD_TASK_DISPLAY: dead_task_name = "display"; break;
		case WD_TASK_KEY: dead_task_name = "key"; break;
		case WD_TASK_LED: dead_task_name = "led"; break;
		default: dead_task_name = "unknown"; break;
	}
	LOG_INFO("BOOT", "reset_reason=%s boot_count=%lu last_dead_task=%s",
		PlatformBackup_ResetReasonToString(reason),
		(unsigned long)boot_count,
		dead_task_name);
	if (reason == BOOT_REASON_SOFTWARE)
	{
		SwResetSource sw_src = (SwResetSource)PlatformBackup_Read(BKP_IDX_SW_RESET_SRC);
		LOG_INFO("BOOT", "sw_reset_src=%s(%lu)",
			PlatformBackup_SwResetSourceToString(sw_src),
			(unsigned long)sw_src);
		if (sw_src == SW_RESET_SRC_HARDFAULT)
		{
			uint32_t bkp_pc = PlatformBackup_Read(BKP_IDX_FAULT_PC);
			uint32_t bkp_lr = PlatformBackup_Read(BKP_IDX_FAULT_LR);
			uint32_t bkp_cfsr = PlatformBackup_Read(BKP_IDX_FAULT_CFSR);
			uint32_t bkp_hfsr = PlatformBackup_Read(BKP_IDX_FAULT_HFSR);
			LOG_ERROR("FAULT", "bkp hardfault pc=0x%08lX lr=0x%08lX cfsr=0x%08lX hfsr=0x%08lX",
				(unsigned long)bkp_pc,
				(unsigned long)bkp_lr,
				(unsigned long)bkp_cfsr,
				(unsigned long)bkp_hfsr);
		}
		PlatformBackup_Write(BKP_IDX_SW_RESET_SRC, (uint32_t)SW_RESET_SRC_UNKNOWN);
	}
	if (reason == BOOT_REASON_IWDG)
	{
		(void)snprintf(msg, sizeof(msg), "event=wdg_reset dead_task=%s", dead_task_name);
		(void)Storage_WriteEvent(msg);
	}

	if (PlatformBackup_Read(BKP_IDX_FAULT_PENDING) != 0U)
	{
		LOG_INFO("BOOT", "fault_pending=1");
		FaultSnapshot snap = g_fault_snapshot;
		uint32_t crc = snap.crc32;
		snap.crc32 = 0U;
		if (snap.magic == FAULT_MAGIC && crc == fault_crc32((const uint8_t *)&snap, (uint32_t)sizeof(snap)))
		{
			LOG_ERROR("FAULT", "hardfault pc=0x%08lX lr=0x%08lX cfsr=0x%08lX hfsr=0x%08lX",
				(unsigned long)snap.stacked_pc,
				(unsigned long)snap.stacked_lr,
				(unsigned long)snap.cfsr,
				(unsigned long)snap.hfsr);
			(void)snprintf(msg, sizeof(msg), "fault type=hardfault pc=0x%08lX lr=0x%08lX cfsr=0x%08lX", (unsigned long)snap.stacked_pc, (unsigned long)snap.stacked_lr, (unsigned long)snap.cfsr);
			(void)Storage_WriteFault(msg);
			LOG_INFO("STORAGE", "fault saved seq=queued");
		}
		PlatformBackup_Write(BKP_IDX_FAULT_PENDING, 0U);
	}
}

void FaultDiag_TestInjectOnce(void)
{
#if ENABLE_HARDFAULT_INJECTION
	static uint8_t injected = 0U;
	volatile uint32_t *bad;
	if (injected != 0U)
	{
		return;
	}
	injected = 1U;
	LOG_WARN("FAULT", "hardfault injection enabled");
	bad = (volatile uint32_t *)0xFFFFFFF1UL;
	*bad = 0xDEADBEEFU;
#endif
}
